# Dual Stacks: Trace Analyzer Handoff Document

This document serves as a technical handoff for the DynamoRIO Trace Analyzer (`tools/trace_analyzer`), detailing its core architecture, performance implications, and the solutions implemented to resolve edge cases encountered during data collection on PARSEC and GAPBS workloads.

---

## 1. Core Architecture

The Trace Analyzer is built on top of **DynamoRIO** and is split into two primary layers:
1. **The Instrumentation Client (`dr_client.cpp`)**: Responsible for weaving into the application's instruction stream, tracking thread lifecycles, and managing thread-local storage (TLS).
2. **The Analysis Engine (`analyzer.cpp`)**: Responsible for maintaining the shadow stack, tracking memory operations, and classifying Stack IPC bounds.

### 1.1 Memory and Thread Tracking
- **Thread Local Storage (TLS)**: Every thread is assigned a private TLS slot upon creation (`event_thread_init`). This slot holds a pointer to an `analyzer_thread_t` context, which maintains the thread's private shadow stack.
- **Instruction Parsing**: The client intercepts basic blocks (`event_app_instruction`), filters for memory operations (Loads/Stores), and specifically identifies Stack operations (Pushes/Pops) based on base register offsets (e.g., `%rsp`).
- **Clean Calls**: The client inserts "clean calls" into the application execution stream to invoke `cb_log_memory_access` and stack-tracking functions in `analyzer.cpp`.

### 1.2 Two-Level Shadow Memory Architecture
To track every single byte of memory dynamically while retaining deep historical metadata (e.g., lifetimes, accessing PCs), the analyzer implements a highly optimized two-level memory shadowing system.

**L1: Fast-Path Shadow Directory (Byte-level Resolution)**
- The L1 structure tracks the owner of every memory byte in the 48-bit address space.
- It is implemented as a 2-level page table to minimize memory footprint for sparse access patterns.
  - **Top Directory**: An array of 2^18 entries mapping 1GB virtual memory chunks.
  - **Shadow Pages**: When a chunk is accessed, a 2GB page of `shadow_info_t` structs is `mmap`'d into memory.
- **`shadow_info_t`**: A 2-byte struct tracking each application byte:
  - `owner_and_flags` (1 byte): Stores the ID of the thread that originally pushed the byte. The Most Significant Bit (MSB `0x80`) is used as a `SHARED_TAG` to quickly mark if *any* other thread has ever accessed this byte (retroactive IPC).
  - `offset` (1 byte): Stores the byte's offset from the base address of its allocation. This allows any random access (e.g., `ld` at `base + 4`) to quickly trace back to the base address and find its extensive L2 metadata.

**L2: Deep Metadata Hash Table (Allocation-level Resolution)**
- While L1 is fast and byte-granular, it is too small to store rich tracking data. Upon every stack allocation (Push), a heavy L2 structure (`addr_detail_t`) is allocated.
- L2 is a highly concurrent Hash Table with 1,048,576 buckets (`L2_HASH_BITS 20`) protected by fine-grained striping (`NUM_L2_LOCKS 4096`).
- **`addr_detail_t`**: Stores deep historical state for the allocation block:
  - Base address, total size, and owner Thread ID.
  - `created_clock`: The owner thread's logical clock timestamp at allocation. When the address is popped or retroactively tainted, this timestamp is subtracted from the current clock to compute the lifetime, populating the global `global_lifetime_histogram`.
  - `pc_list`: A dynamically chained list of `pc_access_t` structs, recording every unique Program Counter (and context hash) that accessed the memory.

### 1.3 IPC Taint Tracking
The analyzer categorizes stack accesses to determine how much of the stack data is truly private versus shared across threads (IPC).
- **Provably Private**: Stack data accessed exclusively by the thread that allocated it.
- **Strictly IPC (Instance Level)**: A stack address allocated by Thread A and accessed by Thread B.
- **Possibly IPC (History / Non-History)**: Tracked via the `g_ipc_taint` global hash map. If an instruction (PC) is observed performing IPC once, subsequent accesses by that PC are tainted as *Possibly IPC*. The history-based metric decays this taint using a histogram approach.

---

## 2. Challenges & Solutions

During large-scale data collection on PARSEC and GAPBS workloads, we encountered several complex edge cases primarily related to DynamoRIO's execution environment and multi-process workloads.

### 2.1 The Timeout Problem (Code Cache Deadlocks)
**Problem**: The heavy instrumentation (tracking every single memory operation) caused immense overhead (50x - 100x slowdowns). Benchmarks like `bodytrack` and `canneal` would hit our timeout bounds. However, standard OS signals (`SIGINT`, `SIGTERM`) and even `dr_register_nudge_event` failed to force the application to dump stats. Threads executing tightly bound loops inside the DynamoRIO code cache could not receive signals promptly.
**Solution**: We bypassed OS signal routing by implementing an **Out-of-Band IPC Timeout Mechanism**. 
- `dr_client.cpp` spawns a native background thread via `dr_create_client_thread()`.
- This native thread opens a named pipe (FIFO) at `-fifo_path` using `O_RDWR` and performs a blocking `read()`. Because it's a native thread, it sleeps directly in the OS kernel space, consuming 0 CPU and completely bypassing the DynamoRIO code cache restrictions.
- When the Python profiling script times out, it opens the FIFO in `O_WRONLY | O_NONBLOCK` mode and pushes bytes. The background thread wakes up, calls `dump_stats(true)`, and forces an immediate teardown via `dr_abort_with_code(1)`.

### 2.2 Multi-Process Wrappers & FIFO Starvation
**Problem**: Workloads managed by `parsecmgmt` are wrapped in intermediate bash scripts. Because we inject DynamoRIO via the `-s` flag, the instrumentation attached to **both** the bash wrappers and the target workload. When the Python script sent a single `T` byte to the FIFO upon timeout, the bash script consumed the byte, dumped its mostly empty stats, and died. The target workload was starved of the shutdown signal and hung indefinitely.
**Solution (The FIFO Flood)**: We modified the Python script to flood the FIFO with 100 `T` bytes (`os.write(fd, b'T' * 100)`). This ensures that every active DynamoRIO client connected to the pipe—whether it's `parsecmgmt`, `tee`, or `bodytrack`—can pull a byte, dump its stats, and cleanly exit.

### 2.3 Identifying the Target Application
**Problem**: Because multi-process applications dumped multiple `=== Stack Privacy Analysis ===` blocks into the same merged `stderr` stream, our parsing script occasionally scraped the stats of a fast-exiting wrapper (e.g., `mkdir` in `facesim`) instead of the actual workload, yielding `0` or heavily truncated buckets.
**Solution**: 
- **Application Tagging**: We modified `dump_stats()` to output `[APP: <name>]` using `dr_get_application_name()`.
- **Heuristic Parsing**: We updated `03_profile_workloads.py` to parse *all* stat blocks in the output stream, and explicitly select the block containing the **highest number of Total Memory Operations**. This cleanly filters out shell wrappers (which perform thousands of operations) and consistently isolates the main workload (which performs millions/billions).

---

## 3. Data Extraction and Google Sheets 
The output of `04_summarize_results.py` has been explicitly tuned to support copy-pasting directly into Google Sheets using the "Split text to columns" feature (Space delimiter).
- Workloads that terminate early via the FIFO timeout append a `*` to the internal application name (e.g., `gapbs_bc(bc*)`).
- Application names and status flags are consolidated into a single contiguous string (e.g., `parsec_blackscholes(blackscholes*)`) to prevent space delimiters from misaligning spreadsheet columns.
