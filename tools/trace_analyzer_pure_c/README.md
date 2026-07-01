# Trace Analyzer (Dual Stacks)

This tool is a DynamoRIO client designed to perform a highly exact, dynamic stack privacy analysis on multithreaded applications. It monitors all stack accesses and determines whether a memory location allocated by a thread is ever accessed by another thread, reporting Inter-Process Communication (IPC).

## Architecture

The analyzer avoids naive memory chunking and heavy mutex locks by utilizing a dual shadow directory approach coupled with lock-free atomic operations.

### Dual Shadow Directories
- **`shadow_dir` (TID Tracking):** A sparse 3-level software page table mapping every byte of application memory to a 1-byte identifier. This byte stores either `UNACCESSED`, the owning `TID`, or `SHARED_TAG`. 
- **`offset_dir` (Instruction Metadata Mapping):** A parallel 3-level page table storing a 1-byte offset for every application byte. When a thread pushes data to the stack, we record the offset from that byte back to the base address of the `PUSH` or `POP` instruction.

This enables **O(1) Exact Partial Overlap Resolution**: if a thread performs an unaligned read from another thread's stack, the analyzer can instantly derive the base address of the original pushing instruction (`base_addr = curr_addr - offset_dir[curr_addr]`), allowing it to correctly attribute IPC without costly backward scans or false sharing.

### Thread-Safety
To run efficiently under massively multithreaded workloads:
1. **Global Allocation Lock**: A single mutex protecting the rare lazy `calloc` of 64KB shadow pages.
2. **Striped L2 Hash Locks**: An array of 4,096 mutexes for the L2 detail hash table, allowing concurrent L2 tracking updates without high lock contention.
3. **Lock-Free State Transitions**: Byte-level state changes (e.g. `TID` to `SHARED_TAG`) are managed using atomic Compare-And-Swap (CAS) instructions (`__sync_val_compare_and_swap`), allowing multiple threads to safely map the memory space simultaneously.

## Output Metrics

At the end of tracing, the tool will summarize the `Stack Privacy Analysis`:
- **Total Memory Operations:** The number of Loads, Stores, Pushes, and Pops intercepted.
- **Provably Private:** Stack operations (PUSH/POP) that were entirely local to the allocating thread and never touched by another thread.
- **Strictly IPC (Instance Level):** Specific stack instances of a PUSH/POP that were accessed by a foreign thread.
- **Possibly IPC (PC Level):** The aggregated number of pushes originating from Program Counters (PCs) that were flagged for IPC at least once during execution.

## Testing

The tool contains three layers of testing:
1. **`test_analyzer`**: A standalone C program testing algorithmic edge cases such as exact unaligned overlaps, true/false sharing scenarios, temporal memory reuse (a deep call stack that transitions from IPC to Private), and concurrent multithreaded stressing.
2. **Integration Tests (`tests/test_*.c`)**: Deterministic multithreaded C programs run under the DynamoRIO client via `scripts/03_integration.sh` to prove that known IPC events are tracked correctly.
3. **End-to-End Tests**: `scripts/02_test.sh` runs real-world benchmarks like PARSEC (Blackscholes) and GAPBS to verify system scalability and performance.

## Usage Examples

To run the analyzer, you must use the `drrun` executable from the DynamoRIO installation to inject `libanalyzer.so` into the target process.

### Basic Command
```bash
# Path to drrun
DRRUN="../../ext/dynamorio/build/bin64/drrun"
# Path to the trace analyzer client
CLIENT="./build/libanalyzer.so"

$DRRUN -c $CLIENT -- <your_command>
```

### Running a GAPBS Benchmark
To trace the Breadth-First Search (BFS) algorithm from the GAP Benchmark Suite on a synthesized graph with 2^10 nodes for 1 iteration:
```bash
$DRRUN -c $CLIENT -- ../../ext/gapbs/bfs -g 10 -n 1
```


### Running a PARSEC Benchmark
To trace `blackscholes` from the PARSEC benchmark suite using the `simsmall` input, you can pass `drrun` directly to the `parsecmgmt` wrapper script via the `-s` flag:
```bash
cd ../../ext/parsec-benchmark
./bin/parsecmgmt -a run -p blackscholes -c gcc -i simsmall -s "$DRRUN -c ../../tools/trace_analyzer/build/libanalyzer.so --"
```
