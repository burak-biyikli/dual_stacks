# Memory Renaming Documentation

This document outlines the design, implementation, and testing framework for a functional, high-performance Memory Renaming engine in the stock gem5 repository (ext/gem5), targeting the X86 architecture.

*Terminology Note: Because this feature injects speculative values at the Rename stage to achieve 0-cycle latency (rather than stalling loads to prevent ordering violations), we use the academic term Memory Renaming (MR) and refer to its structures as the Memory Renaming Predictor (MRP), Memory Renaming Table (MRT), and the Stack Renaming Predictor (SRP).*

## Part 1, Heritage: The Value Predictor Baseline from XS-GEM5

The foundation of this implementation stems from the `ext/xs-gem5` submodule (the XiangShan gem5 fork). XS-GEM5 implements Memory Renaming as part of a generalized **Value Prediction (VP)** framework, entirely decoupled from the Load/Store Unit (LSU). However, the reference codebase only implements 'Stage-1' which tracks store-load dependencies but explicitly stubs out the value file, hardcoding value injection to return false. Because the later stages are not yet implemented, it is not fully functional.

The XS-GEM5 baseline pipeline modifications were as follows:
 1. Instruction State: `ext/xs-gem5/src/cpu/o3/dyn_inst.hh`
   - A vpResult object and auxiliary flags carry speculative state down the pipeline without consuming a separate physical register file.
   - Contains new `vpResult.speculative`, `vpResult.value`, `vpMisprediction` to handle recovery.
 2. Fetch: `ext/xs-gem5/src/cpu/o3/fetch.cc`
   - Eagerly queries the Value Predictor in Fetch as soon as the `DynInst` is created
 3. Rename: `ext/xs-gem5/src/cpu/o3/rename.cc`
   - Bypasses standard dependencies if a prediction is valid, writing data directly into the physical register file.
   - Updates the Scoreboard that the physical destination register is "ready" which lets younger dependent instructions in the Issue Queue to wake up early and bypass the LSU latency
 4. Load/Store Unit (LSU): `ext/xs-gem5/src/cpu/o3/lsq_unit.cc`
   - Unaware of the VP abstraction. Tracks forwarding via producerStorePC to train the predictor.
 5. Issue Queue (IQ) & Execute: `ext/xs-gem5/src/cpu/o3/issue_queue.cc` & `ext/xs-gem5/src/cpu/o3/iew.cc`
   - When the LSU finishes the load, it broadcasts the real writeback. The IQ compares the real LSU data against the local value. If they mismatch, `inst->vpMisprediction = true` is set.
   - `iew.cc` intercepts the misprediction, and attempts a fine-grained recovery, which on failure triggers a squash (flush).
6.  **Commit:** `ext/xs-gem5/src/cpu/o3/commit.cc`
    - If IEW signaled a squash, Commit executes a full architectural pipeline flush, discarding all younger instructions and redirecting Fetch.
    - Trains the predictor upon successful commits using forwarding metadata from the LSQ.

These Stage-1 changes handle the injection and recovery of Memory Renaming, but the predictor is not trained to actually preform forwarding -- it explicitly stubs out the value file, hardcoding value injection to return `false` for `vpResult.speculative`.

## Part 2, Current Design: Memory Renaming Structures and Pipeline Changes

Building off the blueprint of XS-GEM5 on the base of public Gem5, the current design is a full implementation. It contains the structures and changes  needed for a fully functional, high-performance engine that handles physical register constraints, loop disambiguation, and complex recovery hazards. 

Core logic for the predictor tables and structures is implemented in `ext/gem5/src/cpu/valuepred/`. The logic is heavily decoupled into a header-only templated class (`MemoryRenamingCore`) in `memory_renaming_core.hh`. By templating register and instruction identifier types, it has zero dependencies on native gem5 classes (like `gem5::o3::CPU` or `DynInstPtr`), making it easily testable in a lightweight C++ harness. The integrated gem5 wrapper interface is implemented in `memory_renaming.hh` and `memory_renaming.cc`.

### Part 2.1, Core Memory Renaming Structures


#### 2.1.1  Memory Renaming Predictor (MRP)

The MRP determines *if* any rename should occur and identifies the producer.
  - Indexed By: Load PC.
  - Entry Contents: {Tag, Store PC, Dynamic Offset, ConfidenceCounter, Frozen, RRIP}.
  - Confidence & Freezing: A saturating counter tracks usefulness (with a maximum bound defined by `logMaxConfidence`). Allocations start at `allocationConfidence` (default 1). Forwarding is only permitted if the counter meets or exceeds the `predictionThreshold` (confidence >= 2 and Frozen == false). Correct predictions promote the counter; incorrect predictions or missing stores demote it by `demotionPenalty`. Once confidence hits 0, the entry transitions to Frozen and cannot be promoted again.
  - Reallocation Knob: `reallocationConfidence` balances squash risk against forwarding opportunity when a load changes its producer store PC. When it equals `allocationConfidence`, PC-change reallocation resets confidence; when less than `predictionThreshold`, the entry is frozen.
  - Replacement Policy: Uses a 2-bit Re-Reference Interval Prediction (RRIP) policy. RRIP is reset to 0 on hit for both active and frozen entries (the latter serves as sentinel to prevent eviction while blocking prediction). On allocation, RRIP is set to 2. Eviction scans for RRIP=3; if none, all entries have RRIP incremented.

#### 2.1.2 Memory Renaming Table (MRT)

The MRT holds the speculative data from both static PUSH and dynamic ST instructions.
  - Hybrid Layout: Uses `std::unordered_map<Addr, gem5::CircularQueue<MRTEntry>>` mapping Store PC to a circular history queue, plus an active store tracking `std::deque<ActiveStore>` ordered by sequence number for efficient walk-throughs. This provides O(1) lookup for Store PC key and fast retrieval of the instance at Offset.
  - Entry Contents: `{uint64_t data, PhysRegIdPtr st_physreg, InstSeqNum seqNum, bool dataReady, bool valid, uint8_t size}`.
  - Bounded Size: Configured via `mrtTableCapacity` (default 1024) using a FIFO key queue (`mrtKeysFIFO`) to evict oldest store PCs when capacity is exceeded.
  - Squash Rollback: Squashed store instructions represent a contiguous suffix at the write-pointer/tail of the circular queue. On squash, we traverse `activeStoreList` in reverse order for the squashed thread, rolling back (modulo capacity) the MRT queues for only the exact PCs that had active stores squashed. This achieves O(squashed stores) complexity, bypassing the need to scan all 1,024 queues in `mrtTable`.
  - `pendingPhysRegWrites` multimap: Indexes which MRT entry sequence numbers are waiting for which physical register for O(1) writeback updates.

#### 2.1.3 Stack Renaming Predictor (SRP-Stack)

Tracks stack frame context for explicit static pairing.
  - Maintains a logical LIFO stack per thread.
  - Entry Contents: `{Push PC, st_physreg, InstSeqNum seqNum, uint8_t size, int64_t spOffset, bool popped, InstSeqNum poppedSeqNum}`.
  - Stack POP operations match top-down; Stack PEEK operations match by exact spOffset.
  - Global Override: The MRP acts as the ultimate source of truth. If an SRP pairing maps to a load PC that the MRP has frozen, forwarding is suppressed.
  - Data Structure: Implemented using `std::deque` to efficiently support both LIFO operations at Rename and O(1) front pops when matched pops commit, preventing memory leaks.


#### 2.1.4 Retired Store FIFO (RS-FIFO)

Decouples memory renaming training from the Load/Store Unit's cache-drain timing.
 - Maintained at the Commit stage, storing Store PC, sequence number, physical address, and size.
 - Capacity is controlled by committedStoreQueueCapacity.
 - Independently implements a scanning window for loads that arrive at Commit without LSU forwarding metadata (e.g. when stores commit fully before loads execute, the LSU sees no forwarding).
 - To ensure architectural safety, this fallback path only trains if it finds an exact match for both the load's `physAddr` and `accessSize`.
 - Because this structure represents architectural state and is modified exclusively at commit time, there is no need to evict entries on a squash.
 
### Part 2.2, Memory Renaming Pipeline changes

Modifications span several files in src/cpu/o3/ and src/cpu/valuepred/

#### 2.2.1 Commit (`ext/gem5/src/cpu/o3/commit.cc`) - Training & Cleanup
**Training (Overlap Detection & Predictor Updating):**
When a Load retires (commits), we check if there is an active store-load forwarding event and update the MRP. LSU-provided producer metadata (stored in `head_inst->_producerStorePC` and `_producerStoreSeqNum` during forwarding) is packaged into a `ProducerInfoExt` extension and passed to the predictor. If absent, the predictor independently scans the RS-FIFO as a fallback path to detect overlaps. This training logic covers standard dynamic memory forwarding, stack pops, and stack *peeks* (loads reading stack values without popping them).

* **Size Constraint:** For both dynamic and static forwarding, training and entry updates are only performed if the load (Pop/Peek) size strictly matches the store (Push) size. Mismatched size forwarding is suppressed and treated as no forwarding.
* **Case 1: Forwarding Event and No Existing Entry**:
    Allocate a new entry in the MRP for `Load PC -> {Store PC, Offset, ConfidenceCounter = 1}`.
* **Case 2: Forwarding Event and Entry Already Exists**:
    Compare the current `Store PC` against the entry's stored `Store PC`:
    * *If they match:* Increment the `ConfidenceCounter` by 1 (saturating at the maximum value, e.g., 3).
    * *If they mismatch:* Reset the entry to point to the new `Store PC` and reset `ConfidenceCounter = 1` (low confidence).
* **Case 3: No Forwarding Event and Entry Already Exists**:
    Decrement the entry's counter by a configurable demotion penalty (knob). If `ConfidenceCounter` drops to `<= 0`, set it to `0` and freeze the entry (disabled permanently).

*(No squash cleanup of speculative memory renaming tables is required at Commit; it is performed immediately at Rename to avoid timing hazards.)*

#### 2.2.2 Rename (`ext/gem5/src/cpu/o3/rename.cc`) - Dual-Path Prediction & Size Matching
**Dual-Path Lookup:** Rename supports two tunable paths:
1.  **Dynamic:** Queries the MRP with `Load PC` to find the predicted `{Store PC, Offset}`.
2.  **Static:** Explicitly decodes stack Push/Pop/Peek operations on X86 using a **Stack Renaming Predictor (SRP-Stack)**:
    *   **Static uop Size Retrieval at Rename**:
        We can determine the memory access size and base register of any micro-op at the Rename stage from its `StaticInst` pointer:
        ```cpp
        auto *ldst = dynamic_cast<const X86ISA::LdStOp *>(inst->staticInst.get());
        auto *ldstFp = dynamic_cast<const X86ISA::LdStFpOp *>(inst->staticInst.get());
        RegIndex baseReg = ldst ? ldst->base : (ldstFp ? ldstFp->base : X86ISA::int_reg::NumArchRegs);
        uint8_t size = ldst ? ldst->dataSize : (ldstFp ? ldstFp->dataSize : 0);
        int64_t disp = ldst ? ldst->disp : (ldstFp ? ldstFp->disp : 0);
        ```
        This mechanism operates at Rename and ensures size and offset matching is enforced early on both the dynamic and static pathways. The base register index `baseReg` is compared against stack/frame registers `X86ISA::int_reg::_RspIdx` and `X86ISA::int_reg::_RbpIdx`.
    *   **Stack Op Identification & Macro-op Disambiguation**: To minimize modifications to Fetch/Decode stages and keep changes isolated to Rename, stack operation classification is done directly in the Rename stage. Since X86 macro-ops are split into micro-ops, the O3 pipeline natively carries the parent macro-op pointer `inst->macroop` (a `StaticInstPtr`).
        *   **Pop/Push Disambiguation:** Differentiating stack popping operations (e.g. `POP`, `RET`) from generic stack memory accesses (e.g. `mov 8(%rsp), %rax`, which are treated as stack peeks) is done by checking `inst->macroop->getMnemonic()`.
        *   **Classification Rules:**
            *   *Push:* If `inst->isStore()` and `inst->macroop` is not null, it is classified as a push if `inst->macroop->getMnemonic()` is `"push"` or `"call"`.
            *   *Pop:* If `inst->isLoad()` and `inst->macroop` is not null, it is classified as a pop if `inst->macroop->getMnemonic()` is `"pop"` or `"ret"`.
            *   *Store / Peek:* If the base register is the stack pointer (`X86ISA::int_reg::_RspIdx`) or frame pointer (`X86ISA::int_reg::_RbpIdx`) but does not match the Push/Pop mnemonic checks above, it is treated as a regular stack store or stack peek respectively.
    *   **SRP Tracking & Pairing**: Using the `SRP-Stack`, stack pops are paired sequentially with the most recent matching push. Stack peeks are matched against push entries on the SRP-Stack that target the exact same stack pointer offset (`spOffset`).
    *   **Size Matching Enforcement**: 
        - *Dynamic Path:* Rename compares the load's `dataSize` (from `LdStOp`) with the store's `size` field stored in the MRT. Forwarding is suppressed if sizes mismatch.
        - *Static Path:* Forwarding only occurs if the Pop/Peek load size is strictly equal to the Push/Store size on the SRP-Stack. If there is a size mismatch, forwarding is suppressed.
    *   **Global MRP Override**: To prevent problematic static pairings (such as atomics/shared locations) from causing repeated squashes, the **MRP acts as the global source of truth**. When the SRP produces a static pairing for a `Pop PC` or `Peek PC`, it queries the MRP: if the MRP contains a frozen/disabled entry (confidence = 0) for that load PC, static forwarding is suppressed. Static pairings also allocate/train MRP entries, allowing the MRP to learn and disable them if they cause squashes.

**Store Handling:**
*   Every Store passing through Rename determines its access size by casting its static instruction:
    ```cpp
    auto *ldst = dynamic_cast<const X86ISA::LdStOp *>(inst->staticInst.get());
    auto *ldstFp = dynamic_cast<const X86ISA::LdStFpOp *>(inst->staticInst.get());
    uint8_t size = ldst ? ldst->dataSize : (ldstFp ? ldstFp->dataSize : 0);
    ```
*   It allocates an entry in the MRT history queue for its `Store PC` (pushing older entries down), stores its `seqNum`, sets `dataReady = false`, records its `size`, and records the physical register of its data source (`st_physreg`).

**Load Handling & Physical Register Safety:**
*   *Hazard Note:* A Load cannot simply use the Store's physical data register (`st_physreg`) as its own destination. If the LSU executes the Load and corrects a misprediction, it would overwrite that physical register, permanently corrupting the Store's data! Rename **always** allocates a new, unique physical register for the Load (`ld_physreg`).
*   **Case A: Data is Ready (Immediate Injection)**
    *   If `dataReady == true` in the MRT, Rename copies the `data` directly into `ld_physreg` (using `inst->setRegOperand(..., data)`).
    *   The Scoreboard marks `ld_physreg` as ready, instantly waking dependent instructions.
*   **Case B: Data is Not Ready (Logical Pending Queue)**
    *   If `dataReady == false`, the Load registers itself in a **Pending Load Queue** inside the Memory Renaming unit. To allow direct dynamic instruction metadata modification when values become ready, the queue maps the store's physical register to the Load's `DynInstPtr`: `st_physreg -> list of pending load DynInstPtrs`.
    *   The Load is dispatched to the standard LSU to execute in the background.
    *   **Writeback Notification Hook:** To notify the predictor when the Store's producer instruction completes and writes back to `st_physreg`, a custom hook `void writebackReg(PhysRegIdPtr physReg, RegVal val)` is exposed by the `VPUnit`/`MemoryRenaming` class and called from `Scheduler::bypassWriteback` in `ext/gem5/src/cpu/o3/issue_queue.cc` (or IEW writeback stage).
    *   When the hook fires for a physical register:
        1.  The Memory Renaming unit leverages the `pendingPhysRegWrites` multimap to instantly locate the pending store and update the MRT entry's `data` with the written value and sets `dataReady = true`.
        2.  It looks up the `st_physreg` in the Pending Load Queue. For each pending Load `load_inst`:
            - It sets `load_inst->vpResult.value = val` and `load_inst->vpResult.speculative = true`.
            - It writes `val` to `ld_physreg` (retrieved via `load_inst->renamedDestIdx(0)`) using `cpu->setReg(...)`.
            - It marks `ld_physreg` ready in the Scoreboard, waking up the Load's dependents.
        3.  The loads are cleared from the Pending Load Queue for that `st_physreg`.
    *   **Physical Register Lifetime Safety:**
        *   `st_physreg` is safe from premature deallocation. Because the Store is instruction-wise older than the Load, `st_physreg` is guaranteed to remain allocated until a younger instruction that overwrites the Store's source logical register commits. Since the Load is younger than the Store, the Load's writeback and validation occur well before this deallocation point.
        *   **Cleanup on LSU Completion:** If the LSU finishes and writes `ld_physreg` *before* the Store's writeback occurs, the Load must be removed from the Pending Load Queue to prevent the subsequent Store writeback from overwriting the correct LSU value. To do this in O(1) time, we cache the store's `producerPhysReg` flat index inside `vpResult` in the Rename stage. When `completeLoadLSU` is called, it queries the `pendingLoads` map using this cached index directly instead of scanning all entries.
        *   **Cleanup on Squash:** On any squash, any entries in the Pending Load Queue with `load_seqNum > squashed_sn` are invalidated and removed.

**Squash Rollback (MRT, SRP-Stack, & Pending Queue):**
To avoid timing hazards where newly renamed instructions speculatively match stores from the squashed path, rollback of speculative memory renaming tables must occur immediately at the Rename stage squash (`Rename::squash` / `Rename::doSquash`):
*   **MRT Rollback:** Roll back the tail pointer of the `CircularQueue` for each squashed store instruction by traversing the `activeStoreList` in reverse order for the squashed thread, popping the tail of the target PC queue for only those stores where `seqNum > squashed_sn`. This is O(squashed stores) instead of iterating over the entire `mrtTable` key-space.
*   **SRP-Stack Rollback:** Pop and discard entries from the LIFO `SRP-Stack` where `seqNum > squashed_sn` to restore correct stack state.
*   **Pending Load Queue Rollback:** Invalidate and remove any pending load where `load_seqNum > squashed_sn` or where the producer store's physical register (`st_physreg`) was allocated to a squashed instruction.

#### 2.2.3 Load/Store Unit (LSQ)
* **- Execution & Value Verification:** The LSU executes the Load natively in the background. During load writeback (`LSQUnit::writeback`), it reads the real value from the physical register, stores it in `inst->actualValue`, and compares it against `inst->vpResult.value`. If they diverge, it immediately flags `inst->vpMisprediction = true` and triggers `iewStage->squashDueToValuePrediction()`.
* **Queue Cleanup:** It calls `completeLoadLSU` on the Memory Renaming unit upon load completion, which uses the cached `producerPhysReg` in `vpResult` to remove the load from the pending queue in O(1) time, preventing late store writebacks from corrupting the correct LSU data.
* **Training Hooks:** During LSU forwarding, the store PC and sequence number are captured in `_producerStorePC` and `_producerStoreSeqNum` fields on the load instruction via `setProducerStorePC/SeqNum`. At Commit, these are packaged into a `ProducerInfoExt` extension to train the MRP.

### 2.2.4 Issue Queue (`ext/gem5/src/cpu/o3/issue_queue.cc` & `ext/gem5/src/cpu/o3/iew.cc`) - Verification & Recovery
* **Dispatch Wakeups:** When a successfully predicted, data-ready speculative load is dispatched, IEW triggers `lvpWakeDependents()` to instantly update the scoreboard and wake dependent instructions.
* **Register Wakeups:** A new `wakeRegisterDependents()` method in the Instruction Queue allows delayed speculative values (Case B) to wake sleeping dependents immediately upon register writeback.
* **Scoreboard Guarding:** The `InstructionQueue::addToProducers` logic is guarded so it does *not* mark a destination register as unready if the incoming instruction is a speculative load that has already had its data injected. This prevents a critical lost-wakeup deadlock.
* **Recovery ("Squash-Inclusive Recovery"):**
    *   On value prediction misprediction, the load AND all younger instructions are squashed (`includeSquashInst = true`), ensuring the load is re-executed by Fetch redirection to the load's PC.
    *   **Native O3 Support:** Setting `toCommit->includeSquashInst[tid] = true` and `toCommit->valuePredictionError[tid] = true` signals Commit to squash from the load itself and redirect Fetch.
    *   **Early Livelock Prevention:** IEW directly calls `mr->forceFreeze(inst->pcState().instAddr())` in the same cycle the misprediction is caught (`squashDueToValuePrediction`), ensuring the entry is frozen before re-fetch.

#### 2.2.5 SimObject Wiring & Instruction State (`ValuePredictor.py` & `dyn_inst.hh`)
**Configuration Wiring:** The entire Memory Renaming structure is exposed to gem5's Python configuration scripts via a new `ValuePredictor.py` SimObject. This allows dynamic tuning of structural sizes (`ways`, `logESTBEntrys`, `mrtTableCapacity`), behavior (`predictionThreshold`, `reallocationConfidence`), and specific path toggles (`enableDynamic`, `enableStatic`) without recompiling. The `BaseO3CPU` binds to this predictor through a new `valuePred` parameter.

**Instruction Metadata (`dyn_inst.hh`):** To avoid consuming physical registers for speculation, `DynInst` is extended with several helper fields:
  * `vpResult`, `vpRecord`, `actualValue`, `vpSupported`, and `vpMisprediction` carry speculative state.
  * `_producerStorePC` and `_producerStoreSeqNum` fields natively track forwarding origins for training.
  * A strict filter, `canLVP()`, is introduced to ensure that only non-vector loads (`isLoad() && !isVector()`) are eligible for value prediction.

**Predictor Statistics:** The base `VPUnit` class natively tracks rename-time stats (`VPRenameSupported`, `VPRenamePredicted`) and commit-time architectural stats (`VPCorrect` (TP), `VPMispredict` (FP), `VPMissedOpportunity` (FN), and `VPCorrectReject` (TN)), along with derived metrics (`VPaccuracy` (precision) and `VPcoverage` (recall)). To ensure accurate statistics on the speculative wrong path, `VPMispredict` uses a path-sensitive tracking mechanism in Commit to filter out squashed speculative mispredictions.
 
## Part 3, Testing, Verification, and Data Collection Ecosystem

The test suite validates the architecture across **five** distinct layers, managed by the test driver (`scripts/02_test.sh`).

## Part 3.1, Layer 1: Component Unit Tests

These execute the decoupled algorithmic engine (MemoryRenamingCore<int>) directly without gem5 CPU overhead.
 - Validates MRP confidence promotion/saturation/demotion, and 2-bit RRIP replacement.
 - Validates Stack access classification, SRP LIFO pop pairing, and offset-based peek matching.
 - Validates MRT history queue circular boundary logic and squash rollbacks.
 - Build Integration: A new `SConscript` file was added to `src/cpu/valuepred/` to register the `MemoryRenaming` SimObject and compile the isolated GTest binaries (`memory_renaming.test.opt`, etc.) alongside the main simulator.
   
## Part 3.2, Layer 2: Pipeline Harnesses

Lightweight testing harnesses mirror the exact contracts of the Rename and Commit stages.
 - Rename Harness: Validates Case A (Immediate), Case B (Pending Writeback), multiple pending loads, LSU race conditions, and static SRP paths.
 - Commit Harness: Validates the RS-FIFO sliding window, successful/unsuccessful MRP training events, and deferred misprediction squash signals.   
   
## Part 3.3, Layer 3: O3 CPU Microbenchmarks

Executes bare-metal X86 assembly (*.S) on stock vs. MR-enabled gem5 models.
 - Success Criteria: Simulations must complete; Stock must report 0 VP activity; MR must match expected stats for `VPRenameSupported`, `VPRenamePredicted`, `VPCorrect`, and `VPMispredict` (verified via `check_stats.py`); Execution traces must be identical and free of corruption (normalized and verified via diffing `trace_stock_clean.txt` and `trace_mr_clean.txt` via `parse_traces.py`).
 - Coverage: Covers multi-iteration forwarding delays, RPN stack pushing/popping, changing dependencies (inducing safe squashes), and negative tests (size mismatches, alternating PC dependencies) that explicitly ensure prediction correctly freezes. Simulation artifacts, standard out, and trace diffs are automatically routed to `/tmp/gem5_dual_stacks_tests/<test_name>/`.
   
## Part 3.4, Layer 4: Workloads as Macrobenchmarks

In addition to handwritten microbenchmarks, we also can use the workloads to test parity between models as part of our testing flow similar to the microbenchmarks.
 - Initialization: `01_setup.sh` pulls submodules and builds all dependencies (DynamoRIO, GAPBS, gem5). `02_test.sh` acts as a health check, executing "Hello World" sanity validations across all tools.
 - Hybrid Stress Test Loop: Integrates macrobenchmarks (currently `bfs` and `blackscholes`) into the automated testing suite (`run_tests.sh`). Execution enforces `OMP_NUM_THREADS=1` in the environment to prevent OpenMP thread explosion. They run in two phases: "Trace Mode" (capped at 100K instructions for trace divergence validation) and "Stress Mode" (capped at 15M instructions, no tracing, for deep pipeline execution stress).
 
## Part 3.5, Layer 5: Workloads

The environment leverages a sequential suite of bash and python scripts (00 through 05) alongside DynamoRIO and PARSEC/GAPBS workloads to test high-level objectives.
 - Automated Profiling: Workloads are executed via `03_profile_workloads.py` and parsed by `04_summarize_results.py`. Because heavy C++ workloads (like `bodytrack`) take a long time under dense tracing, the profiler supports a `--timeout <seconds>` flag that safely intercepts `SIGTERM` to gracefully flush hash tables and save collected data. Finally, macrobenchmark simulations are managed by `05_run_simulations.py`.
 - Synthetic vs. Real-World: The pipeline supports `-g` synthetic Kronecker graphs for lightweight functional testing, but real-world GAP datasets (e.g., GAP-twitter) from the SuiteSparse Matrix Collection are recommended for robust architectural profiling to avoid artificially high cache efficiencies.

## Part 3.6, Verification Results

* **Unit Tests**: All 15 core tests, 8 rename pipeline tests, and 16 commit pipeline tests pass successfully.
* **Microbenchmarks**: All 12 assembly microbenchmarks pass successfully.
* **Macrobenchmarks**: Simulation macrobenchmarks (`bfs` and `blackscholes`) run to completion successfully under the hybrid stress test harness, and trace comparisons verify perfectly with zero timing/deterministic mismatches.
 
## Part 4, Error Log: Bug Fixes & Enhancements

During testing and implementation, the following critical bugs were resolved. This section catalogs issues incase they are reintroduced inadvertently.

### Part 4.1: Incomplete Type Errors in `comm.hh`
* **Issue:** Adding in-class initializers (e.g. `= {}` or `= 0`) to stage communication structs (like `FetchStruct`, `DecodeStruct`, etc.) in `comm.hh` forced the compiler to instantiate constructors/destructors in header files before the definition of `DynInst` was complete. This caused compilation failures due to the destructors of smart pointers (`RefCountingPtr<DynInst>`).
* **Fix:** Replaced in-class initializers in [comm.hh](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/comm.hh) with explicit constructor and destructor declarations. Implemented them in [cpu.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/cpu.cc) at the end of the file where `DynInst` is a fully defined type.

### Part 4.2: SCons Caching & Struct Layout Mismatch
* **Issue:** Changes in pipeline headers did not trigger automatic recompilation of dependent stages due to SCons checksum optimization caching, causing alignment and ABI mismatches between `cpu.cc` and stage `.cc` files, resulting in segmentation faults in Fetch queue sorting.
* **Fix:** Manually cleaned/deleted stage object files under `build/X86/cpu/o3/` to force scons to recompile all stages.

### Part 4.3: Missing CPU Initializers
* **Issue:** Key CPU constructor initializers `globalSeqNum(1)` and `globalFTSeqNum(1)` were missing, leading to undefined sequence numbering behavior.
* **Fix:** Restored these initializers in the `CPU` constructor in [cpu.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/cpu.cc).

### Part 4.4: Compiler Dead-Store Optimization in `TimeBuffer` (Segfaults and Simulation Hangs)
* **Issue:** The `TimeBuffer` constructor and `advance()` method allocated memory and called `std::memset(ptr, 0, sizeof(T))` followed by placement new `new (ptr) T`. For POD types (like `bool`, `int` inside the `ActivityRecorder`) or aggregate structs with no user-defined constructors (like `FetchStruct` initially), the compiler optimized away `std::memset` as a dead store. This resulted in time buffers, fetch/decode queues, and the activity recorder starting with garbage values from the heap, causing Decode to segfault on cycle 0 or the CPU to deschedule itself and hang forever in an idle loop.
* **Fix:** Modified [timebuf.hh](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/timebuf.hh) to use value-initialized placement new `new (ptr) T()` in both the constructor (lines 148-149) and `advance()` method (line 189). This guarantees zero-initialization of PODs/aggregates and prevents compiler optimizations from skipping zeroing.

### Part 4.5: Lost-Wakeup Deadlock in Instruction Queue (Simulation Hangs)
* **Issue:** During microbenchmark testing (specifically on `app_rpn_calculator`), the simulation would deadlock/hang indefinitely once the IQ became full. We discovered that when a speculative load was predicted and had its value dynamically injected early via `writebackReg`, it set the destination register as ready in the IQ's internal `regScoreboard`. However, when that load was subsequently dispatched to the IQ, the CPU's default `InstructionQueue::addToProducers` stage marked the destination register as unready (`regScoreboard[dest] = false`). Younger dependent instructions dispatched after this point would see the register as unready and append themselves to the dependency graph. Because the load's value writeback had already occurred, no future writeback/wakeup event was scheduled for that register, causing all dependents to wait indefinitely and starve the pipeline.
* **Fix:** Guarded the scoreboard unsetting in `InstructionQueue::addToProducers` in [inst_queue.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/inst_queue.cc). The destination register is now only marked unready if the instruction is NOT a speculative load that has already had its data successfully injected (`!(new_inst->vpResult.speculative && new_inst->vpResult.dataReady)`).

### Part 4.6: PC-Scoped Producer Offset at Commit
* **Issue:** `getProducerOffset` counted every committed store between the producer and the load, regardless of store PC. When multiple distinct store PCs retired between a load and its producer (e.g. `S1` then `S2` in `mispredict_changing_dependency`), Rename queried `mrtTable[S1]` with a global offset of `1` instead of the correct per-PC offset of `0`. This injected stale iteration data and produced a 100% misprediction rate (`VPcorrect = 0`).
* **Fix:** `getProducerOffset(producerStoreSeqNum, storePC)` now increments its offset counter only for committed stores whose PC matches the producer store PC. Updated in `memory_renaming_core.hh`, `memory_renaming.cc`, and the pipeline test harness.

### Part 4.7: Predictor Freeze from Squash-Induced LSQ Misses
* **Issue:** When older stores retire into the L1 cache before younger loads execute (e.g., due to a squash shadow or a cold cache miss during warmup), loads miss in the LSQ and arrive at Commit without `ProducerInfoExt`. The predictor interpreted this missing metadata as a failed forwarding event and aggressively demoted entries via `trainMRP(false)`, permanently freezing predictable loads. This capped correct predictions in `mispredict_changing_dependency` (e.g., Correct=180 vs. ~1600 expected).
* **Fix:** Added a **Retired Store FIFO (RS-FIFO)** in `MemoryRenamingCore` that records each committed store's PC, sequence number, physical address, and size. At Commit, when LSU forwarding metadata is absent, `findForwardingStore()` independently scans the RS-FIFO for a matching address and size. Matching loads train on the retired store's PC instead of being demoted. Training remains decoupled from LSQ drain timing.

### Part 4.8: Squash and Redirect Wiring (Fixing Pipeline Corruption)
* **IEW Squash in Commit**: Modified [commit.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/commit.cc) to handle `fromIEW->valuePredictionError[tid]` at the beginning of `Commit::commit()`. When a value prediction misprediction is signaled, Commit immediately squashes all instructions starting from the load itself (`includeSquashInst = true`) and redirects the Fetch stage to the load's PC.
* **Removal of Legacy Fallback Squash**: Removed the legacy `valuePredictionSquash` fallback squash and macro-op boundary checks from `Commit::commitInsts()`. The pipeline now flushes speculatively-corrupted instructions prior to commit rather than allowing them to commit and corrupt architectural registers.

### Part 4.9: Early Predictor Freezing & Livelock Resolution
* **Livelock Prevention**: To prevent infinite value prediction/squash loops (e.g. on stack push/pop instructions that bypass normal LSU forwarding), we implemented a `forceFreeze(Addr loadPC)` mechanism in `MemoryRenamingCore` ([memory_renaming_core.hh](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/valuepred/memory_renaming_core.hh)).
* **MRP Allocation on Miss**: If the mispredicting load's PC is not already allocated in the MRP table, `forceFreeze` allocates a new frozen MRP entry using RRIP replacement. This guarantees that subsequent fetches of the instruction will hit a frozen entry and bypass prediction.
* **Early IEW Hook**: Wired `forceFreeze` directly into `IEW::squashDueToValuePrediction` ([iew.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/iew.cc)) to ensure the entry is frozen in the same cycle the squash is initiated.

### Part 4.10: Pipeline Deadlock Resolution
* **Stall Signal Removal**: Removed the unnecessary `toRename->iewBlock[tid] = true;` block from `IEW::squashDueToValuePrediction` ([iew.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/iew.cc)). Standard squashes in O3 CPU do not block stages; setting this block signal had caused Rename to stall permanently and deadlock the simulation.

### Part 4.11: Performance Optimization: O(1) Writeback Indexing
* **Issue:** Previously, `MemoryRenamingCore::updateMRTData` had to loop through the entire `mrtTable` on every single instruction writeback to update data for pending stores. For long-running macrobenchmarks with thousands of unique store PCs, this resulted in an O(unique store PCs × instructions) complexity bottleneck, causing the simulator to hang/freeze for hours on BFS.
* **Fix:** Introduced a `pendingPhysRegWrites` multimap in `memory_renaming_core.hh` to index which MRT entry sequence numbers are waiting for which physical register. Writeback updates now run in O(1) time by querying this map directly, speeding up macrobenchmark simulation from hours to seconds.

### Part 4.12: Added `mock_rdtsc` for Deterministic Timing in hybrid tests
* **Motivation:** Macrobenchmarks (like `bfs` and `blackscholes`) exhibit trace mismatches because the Stock and Memory Renaming CPU models operate on slightly different microarchitectural timelines (cycle ticks). When the guest code runs `RDTSC` (Read Time-Stamp Counter), it returns slightly different cycle numbers, causing architectural registers to diverge and leading to trace validation failures.
* **Implementation:**
  - Added a `mock_rdtsc` SimObject parameter (defaulting to `False`) to [BaseCPU.py](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/BaseCPU.py).
  - Initialized `_mockRdtsc` in the `BaseCPU` constructor in [base.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/base.cc) and declared a getter `getMockRdtsc()` in [base.hh](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/base.hh).
  - Modified the x86 `Rdtsc` micro-op in [regop.isa](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/arch/x86/isa/microops/regop.isa) to check if `mock_rdtsc` is enabled. If true, it returns a static incrementing counter (`dummy_tsc += 100`) to guarantee deterministic timing ticks between runs.
  - Enabled `system.cpu.mock_rdtsc = True` in [configs/run_o3_stock.py](file:///home/bbiyikli/Desktop/dual_stacks/configs/run_o3_stock.py) and [configs/run_o3_mr.py](file:///home/bbiyikli/Desktop/dual_stacks/configs/run_o3_mr.py).

### Part 4.13: Writeback Register Class Guard
* **Issue:** When the CPU writes back to control/miscellaneous registers (such as `TscOp` from `RDTSC` or Segment/Model Specific Registers), `IEW::writebackInsts` called `cpu->getReg(phys_reg)` on a `MiscRegClass` register. Since the physical register file only stores/tracks general-purpose registers (Int, Float, CC, Vector), querying it for a miscellaneous register triggered a simulator panic (`Unsupported register class type 7`).
* **Fix:** Guarded the value predictor register read in [iew.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/iew.cc) to only read/write back registers of `IntRegClass` (`inst->renamedDestIdx(i)->is(IntRegClass)`), since the Memory Renaming unit only operates on integer registers.
  
### Part 4.14: Path-Sensitive Value Misprediction Stats Tracking
* **Issue:** Previously, `VPMispredict` was incremented unconditionally at the squash-processing point in Commit. However, squashes can occur on speculatively wrong paths (e.g. branch mispredictions) that are later squashed by a senior instruction. This caused value prediction stats to count wrong-path speculative load mispredictions, leading to inaccurate accuracy metrics and masking true performance.
* **Fix:** Introduced a path-sensitive verification algorithm:
  - Added `std::set<InstSeqNum> pendingVPMispredicts[MaxThreads]` to `Commit`.
  - When a value prediction error squash occurs, if the instruction immediately preceding the load has committed, increment `VPMispredict` immediately. Otherwise, insert it into the pending set.
  - When any older squash is processed, remove all junior entries from the pending set.
  - When an instruction successfully commits, check if its successor is in the pending set. If so, increment `VPMispredict` and remove it from the set.
  
### Part 4.15: Floating-Point Physical Register Writeback Leak (Timeout in `parsec_vips_test`)
* **Issue:** During simulations of floating-point heavy workloads (e.g. `parsec_vips_test`), the simulation would hit the 600-second timeout while making extremely slow progress. We discovered that `IEW::writebackInsts` only wrote back destination registers of `IntRegClass` to the value predictor, completely omitting `FloatRegClass` destination registers. This caused speculative loads waiting for floating-point store values to never be notified of writebacks, leaking their entries in `pendingPhysRegWrites` and leading to massive CPU execution stalls.
* **Fix:** Modified [iew.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/o3/iew.cc) to write back destination registers belonging to either `IntRegClass` or `FloatRegClass` to the value predictor.

### Part 4.16: O(N) Table Scan on Squash in `MemoryRenamingCore` (Slowdown in `gapbs_tc_test`)
* **Issue:** When squash occurred, `MemoryRenamingCore::squash` iterated over the entire 1,024-entry `mrtTable` key-space to roll back squashed stores. Under workloads with high branch misprediction rates and large working sets (e.g. `gapbs_tc_test`), this O(N) traversal created a major bottleneck, resulting in extreme simulator slowdowns.
* **Fix:** Replaced the O(N) `mrtTable` loop in [memory_renaming_core.hh](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/valuepred/memory_renaming_core.hh) with a LIFO reverse-traversal of `activeStoreList` for the squashed thread. Since `activeStoreList` only contains active in-flight stores, we now only roll back the queues of PCs that were actually squashed.

### Part 4.17: O(N) Search in `completeLoadLSU`
* **Issue:** When the LSU natively completed a load instruction, `completeLoadLSU` scanned all elements in `pendingLoads` to find and erase the corresponding load by its sequence number. This O(N) map search occurred for every load execution in the LSU, incurring significant overhead.
* **Fix:** Optimized the lookup in [memory_renaming.cc](file:///home/bbiyikli/Desktop/dual_stacks/ext/gem5/src/cpu/valuepred/memory_renaming.cc) to O(1) time by caching the store's physical register flat index in `vpResult.producerPhysReg` at Rename. When `completeLoadLSU` is triggered, the predictor performs a hash map `find` directly on the cached index, eliminating full map iterations.

## Part 5, Known Testing Gaps & Future Work

 - IEW squash-after in full O3 is covered by harnesses and indirectly by microbenchmark trace diffing, but direct full ROB/RAT interaction tests are limited.
 - Macrobenchmark studies for PRF pressure sweeps and LSU roofline savings (as outlined in the Design document) are pending integration into the central test driver.
 - There is a need to pull real-world graphs for final evaluations, and investigate extreme slowdowns of specific workloads (e.g., bodytrack) under dense DynamoRIO tracing.
 - Development of the planned `06_extract_data.py` script to complete the automated research pipeline.
