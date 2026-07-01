# Trace Analyzer

This tool measures the proportion of memory operations that could potentially lead to Inter-Process Communication (IPC) by monitoring memory access patterns. Specifically, it analyzes stack privacy, breaking down memory into "provably private", "strictly IPC", and "possibly IPC".

## Architecture

The tool is split into two primary components to enforce modularity and ensure thread safety:

1. **DynamoRIO Client (`dr_client.cpp`)**:
   - Hooks into DynamoRIO to instrument memory accesses (`ld`, `st`, `push`, `pop`).
   - Gathers contextual execution data such as the Program Counter (PC) and the Global History Register (GHR).
   - Generates a `ctx_hash` from the GHR using a MurmurHash3 finalizer to group related execution paths.
   - Passes data into the analyzer backend using `clean_call`s.
   - Handles thread registration, exit tracking, and system signals (e.g., catching `SIGINT` to cleanly dump stats before terminating).

2. **Analyzer Backend (`analyzer.cpp` / `analyzer.h`)**:
   - Manages memory access state and tracks whether addresses have been shared across threads.
   - Utilizes a two-level locking hierarchy:
     - **L2 Table (`l2_locks`)**: Fine-grained locking over chunks of address space (64KB chunks).
     - **PC Banks (`pc_banks`)**: Tracks metrics grouped by the executing PC.
   - Resolves potentially shared accesses into the `Possible IPC` metrics, tracking whether shared behavior occurred under specific contextual histories.

## Testing

The trace analyzer includes a rigorous testing framework located in `tests/`.

To run all tests, simply execute:
```bash
./tests/run_tests.sh
```

### Types of Tests
- **Unit Tests (`test_analyzer.cpp`)**: Pure C++ tests directly interfacing with the `analyzer_on_*` functions to validate the L2 table logic, locking, temporal metrics, and context grouping without the DynamoRIO overhead.
- **Fuzzer (`test_fuzzer_minimal_stress.cpp`)**: Concurrently stresses the `analyzer`'s locking mechanisms to ensure no deadlocks occur under high concurrency.
- **Integration Tests (`test_ipc`, `test_private`, etc.)**: C applications executed under `drrun` to validate the DynamoRIO instrumentation end-to-end.
