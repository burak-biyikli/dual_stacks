# gem5 Memory Renaming Test Suite

This directory validates the Stage-2 Memory Renaming implementation implemented 
for this project. Tests are organized in four layers: isolated C++ unit tests, 
rename-stage pipeline harness tests, commit-stage pipeline harness tests, and 
full O3 CPU microbenchmarks.


================================================================================
RUNNING TESTS
================================================================================

From the repository root or this directory:

    ./tools/gem5_tests/run_tests.sh          # quiet build
    ./tools/gem5_tests/run_tests.sh -v       # verbose scons output

The driver script (`run_tests.sh`):

  1. Rebuilds three GTest binaries plus `gem5.opt` (X86 O3)
  2. Runs all C++ unit and pipeline integration tests
  3. Compiles each `*.S` microbenchmark, runs it under stock and MR gem5
     configs, checks stats, and diffs execution traces

Simulation artifacts are written to:

    /tmp/gem5_dual_stacks_tests/<test_name>/

Helper scripts in this directory:

  check_stats.py   - Parses stats.txt for VPsupported / VPpredicted / VPcorrect
  parse_traces.py  - Normalizes gem5 Exec debug traces for diffing


================================================================================
LAYER 1: COMPONENT UNIT TESTS
================================================================================

Location:
  ext/gem5/src/cpu/valuepred/memory_renaming.test.cc

Build target:
  build/X86/cpu/valuepred/memory_renaming.test.opt

These tests exercise `MemoryRenamingCore<int>` — the decoupled algorithmic
engine in `memory_renaming_core.hh` — without SimObject, DynInst, or CPU
dependencies.

Test                              What it validates
----                              -----------------
MRPConfidenceAndFreezing          Allocation at confidence 1, promotion,
                                  saturation, demotion, freezing, PC-mismatch
                                  reset
MRPConfidenceTuneableDemotion...  Configurable demotion penalty and confidence
                                  ceiling
MRPRRIPReplacementDeterministic 2-bit RRIP: allocation at 2, hit reset to 0,
                                  eviction of non-recent entries
MRPRRIPReplacementFuzzer        Randomized eviction stress (4/8/16/32 ways)
StackAccessClassification       Push/call, pop/ret, peek, stack store, and
                                  non-stack classification via classifyStackAccess()
SRPStackLIFOAndPeek              LIFO pop pairing, offset-based peek matching,
                                  size mismatch suppression
SRPStackFrozenMRPOverride       Frozen MRP entry blocks static SRP forwarding
MRTHistoryQueueAndEviction      Circular queue capacity, offset-0 recency,
                                  overflow eviction
DynamicMRTOffsetSelection       MRP offset field selects correct historical
                                  MRT instance
CommitProducerOffset            Commit-time sliding window returns correct
                                  store-load dynamic offset
DynamicPathSizeMismatch...      8-byte store / 4-byte load does not forward
SquashRollbackMRTAndSRP         MRT tail rollback, SRP push removal, popped-
                                  state restoration


================================================================================
LAYER 2A: RENAME-STAGE PIPELINE HARNESS
================================================================================

Harness header (shared, update here when production hooks change):
  ext/gem5/src/cpu/valuepred/memory_renaming_pipeline_harness.hh

Test file:
  ext/gem5/src/cpu/valuepred/memory_renaming_rename_pipeline.test.cc

Build target:
  build/X86/cpu/valuepred/memory_renaming_rename_pipeline.test.opt

Why a separate harness header?

  Instantiating `MemoryRenaming` directly requires gem5 SimObject
  initialization. `RenamePipelineHarness` mirrors the rename-stage contract
  implemented in `memory_renaming.cc` (renameStore, renameLoad, writebackReg,
  completeLoadLSU, squash) using integer physical register indices and an
  in-memory scoreboard. When the production rename hooks change, update the
  harness methods once; test files stay thin.

Test                              Pipeline scenario
----                              -----------------
CaseAImmediateInjection           Store data ready at rename -> load injected
                                  immediately (Case A)
CaseBPendingWriteback             Store source pending -> load queued ->
                                  writebackReg wakes load (Case B)
MultiplePendingLoads              Two loads pending on one store physreg,
                                  both woken by single writeback
LSUCompletionRace                 LSU completes before store writeback;
                                  pending entry removed; late writeback does
                                  not corrupt LSU value
SquashCleansPendingAndTables      Squash rolls back MRT, SRP, and pending-load
                                  queue together
SRPStaticPath                     Push/pop static pairing without MRP training
WritebackVerificationMispred...   IEW bypassWriteback detects predicted vs
                                  actual value mismatch
RenameCommitIntegration           Rename + commit harnesses wired together for
                                  end-to-end store/load retirement


================================================================================
LAYER 2B: COMMIT-STAGE PIPELINE HARNESS
================================================================================

Harness header (same file as Layer 2):
  ext/gem5/src/cpu/valuepred/memory_renaming_pipeline_harness.hh
  (class CommitPipelineHarness)

Test file:
  ext/gem5/src/cpu/valuepred/memory_renaming_commit_pipeline.test.cc

Build target:
  build/X86/cpu/valuepred/memory_renaming_commit_pipeline.test.opt

`CommitPipelineHarness` mirrors the commit-stage contract in `commit.cc` and
`MemoryRenaming::update()`:

  - commitStore()          -> MemoryRenaming::commitStore()
  - commitLoad()           -> MRP training, SRP retire, VPcorrect accounting,
                             misprediction flagging
  - finishInstBoundary()   -> squash-after on value misprediction

Test                              Commit scenario
----                              ---------------
StoreCommitEnablesProducerOffset  Committed-store sliding window enables
                                  getProducerOffset()
LoadTrainingWithForwarding        Forwarding event promotes MRP confidence
LoadTrainingNoForwardingDemotes   Missing producer demotes / freezes MRP entry
LoadTrainingUnknownProducer...  Producer seqNum not in commit window demotes
StorePCMismatchResetsConfidence Different store PC on hit resets confidence
SRPRetireOnLoadCommit             retireSRP() removes committed popped entries
VPcorrectIncrementsOnSuccess...   VPcorrect stat logic on successful prediction
MispredictionSquashDeferred...    vpMisprediction sets squash pending before
                                  instruction boundary
MispredictionSquashAtBoundary     finishInstBoundary() triggers squash-after
MultiStoreOffsetTraining          Offset-2 producer resolved from commit window
RenameCommitEndToEndMRPThreshold  Full rename+commit loop reaches MRP threshold


================================================================================
LAYER 3: O3 CPU MICROBENCHMARKS
================================================================================

Each subdirectory contains a bare-metal X86 assembly program (`*.S`). The test
driver compiles with `gcc -nostdlib`, then runs under:

  configs/run_o3_stock.py   (baseline, no memory renaming)
  configs/run_o3_mr.py      (memory renaming enabled)

Pass criteria (all must hold):

  1. Both simulations exit successfully
  2. Stock stats: VPsupported, VPpredicted, VPcorrect must all be 0
  3. MR stats: checked by check_stats.py (see table below)
  4. Trace equivalence: parse_traces.py strips tick prefixes; cleaned stock
     and MR exec traces must match (diff identical)

Microbenchmark catalog:

Directory                         Purpose                         MR expectation
---------                         -------                         --------------
app_lfsr_memory_pattern/          Four kernels at store-load       VPpredicted > 0
                                  distances 1/2/4/50
app_rpn_calculator/               RPN stack calculator —           VPpredicted > 0
                                  repeated push/pop SRP + MRP
forwarding_between_loop_iters/    Setsup forwarding to occur       VPpredicted > 0
                                  between loads in the current iter
                                  against stores in the current,
                                  prior, and two iters prior
forwarding_delayed_copy/          add -> mov store (not ready)     VPpredicted > 0
                                  -> load (Case B pending queue)
forwarding_immediate_injection/   Stack store + delay + load       VPpredicted > 0
                                  (Case A immediate injection)
forwarding_multiple_pending...    Two loads pending on one         VPpredicted > 0
                                  not-ready store
forwarding_stack_peek/            push + mov (%rsp) peek           VPpredicted > 0
                                  (static SRP path)
mispredict_changing_dependency/   Similar to the no forwarding     VPpredicted > 0
                                  dependency test, but induces 
                                  squashes intentionally
mispredict_push_store_overlap/    Stack accesses are often safe to VPpredicted > 0
                                  forward, and might be done with
                                  no training. This tests failures
                                  in that path.
no_forwarding_demotion/           Tests a LD<-ST pair which is     VPpredicted == 0
                                  broken up by regular stores. 
                                  This should cause a demotion 
                                  and freeze the entry
no_forwarding_dependency/         Alternating load-store PC        VPpredicted == 0
                                  pairing prevents MRP confidence
no_forwarding_size/               32-bit store + 64-bit load       VPpredicted == 0
                                  size mismatch

================================================================================
ADDING A NEW MICROBENCHMARK
================================================================================

  1. Create tools/gem5_tests/<name>/<name>.S with _start entry and exit syscall
  2. Use a loop (>= 100 iterations) so the MRP can train past the confidence
     threshold (default 3)
  3. Include self-checks (cmp / jne fail) so incorrect forwarding returns
     non-zero exit code
  4. Run ./run_tests.sh -v and inspect /tmp/gem5_dual_stacks_tests/<name>/
     on failure
  5. If the test should NOT predict, add the name to the negative list in
     check_stats.py


================================================================================
ADDING A NEW PIPELINE OR UNIT TEST
================================================================================

Component algorithm test (no pipeline):
  Add to memory_renaming.test.cc

Rename-stage pipeline test:
  Add to memory_renaming_rename_pipeline.test.cc
  If the rename hook contract changes, update RenamePipelineHarness in
  memory_renaming_pipeline_harness.hh

Commit-stage pipeline test:
  Add to memory_renaming_commit_pipeline.test.cc
  If the commit hook contract changes, update CommitPipelineHarness in
  memory_renaming_pipeline_harness.hh

Register new GTest binaries in:
  ext/gem5/src/cpu/valuepred/SConscript
  tools/gem5_tests/run_tests.sh  (UNIT_TESTS array)


================================================================================
KNOWN GAPS AND FUTURE WORK
================================================================================

  1. IEW squash-after in full O3: bypassWriteback misprediction detection is
     covered by the rename harness; squash-after recovery is covered by the
     commit harness. Full ROB/RAT interaction is only exercised indirectly via
     microbenchmark trace equivalence.

  2. Macrobenchmark studies (PARSEC/GAPBS LSU roofline, PRF pressure sweeps)
     are planned in Memory_Renaming_Design.md Part III but not yet in this
     test driver.
