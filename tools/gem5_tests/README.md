# gem5 Memory Renaming Test Suite

This directory validates the Stage-2 Memory Renaming implementation described in [`Memory_Renaming_Design.md`](../../Memory_Renaming_Design.md). Tests are organized in three layers: isolated C++ unit tests, mocked rename-stage integration tests, and full O3 CPU microbenchmarks.

## Running Tests

From the repository root (or this directory):

```bash
./tools/gem5_tests/run_tests.sh          # quiet build
./tools/gem5_tests/run_tests.sh -v       # verbose scons output
```

The driver script:

1. Rebuilds `memory_renaming.test.opt` (GTest) and `gem5.opt` (X86 O3)
2. Runs all C++ unit/integration tests
  - Simulation artifacts are written to `/tmp/gem5_dual_stacks_tests/<test_name>/`.
  - `check_stats.py` parses `stats.txt` for `VPsupported` / `VPpredicted` / `VPcorrect` values
  - `parse_traces.py` normalize gem5 Exec debug traces for diffing
3. Compiles each `*.S` microbenchmark, runs it under stock and MR gem5 configs, checks stats, and diffs execution traces

## Layer 1: Component Unit Tests

**Location:** `ext/gem5/src/cpu/valuepred/memory_renaming.test.cc`
**Target:** `MemoryRenamingCore<int>` — the decoupled algorithmic engine in `memory_renaming_core.hh`

These tests exercise predictor/table logic without gem5 `SimObject`, `DynInst`, or CPU dependencies.

| Test | What it validates |
|------|-------------------|
| `MRPConfidenceAndFreezing` | Allocation at confidence 1, promotion to threshold, saturation, demotion, freezing, PC-mismatch reset |
| `MRPConfidenceTuneableDemotionAndClamping` | Configurable demotion penalty and confidence ceiling |
| `MRPRRIPReplacementDeterministic` | 2-bit RRIP: allocation at 2, hit reset to 0, eviction of non-recent entries |
| `MRPRRIPReplacementFuzzer` | Randomized eviction stress (4/8/16/32 ways, 2000 iterations each) |
| `StackAccessClassification` | Push/call, pop/ret, peek, stack store, and non-stack classification via `classifyStackAccess()` |
| `SRPStackLIFOAndPeek` | LIFO pop pairing, offset-based peek matching, size mismatch suppression |
| `SRPStackFrozenMRPOverride` | Frozen MRP entry blocks static SRP forwarding |
| `MRTHistoryQueueAndEviction` | Circular queue capacity, offset-0 recency, overflow eviction |
| `DynamicMRTOffsetSelection` | MRP offset field selects correct historical MRT instance |
| `CommitProducerOffset` | Commit-time sliding window returns correct store-load dynamic offset |
| `DynamicPathSizeMismatchSuppression` | 8-byte store / 4-byte load does not forward |
| `SquashRollbackMRTAndSRP` | MRT tail rollback, SRP push removal, popped-state restoration |

---

## Layer 2: Pipeline Harness

**Location:** `ext/gem5/src/cpu/valuepred/memory_renaming.test.cc`

This addresses the gap between isolated component tests and full CPU simulation. Instantiating `MemoryRenaming` directly requires gem5 `SimObject` initialization, which is impractical for fast unit tests. Instead, `RenamePipelineHarness` mirrors the rename-stage contract that `MemoryRenaming::renameStore`, `renameLoad`, `writebackReg`, `completeLoadLSU`, and `squash` implement — using integer physical register indices and an in-memory scoreboard.

| Test | Pipeline scenario |
|------|-------------------|
| `RenameHarnessCaseAImmediateInjection` | Store data ready at rename → load injected immediately (Case A) |
| `RenameHarnessCaseBPendingWriteback` | Store source pending → load queued → `writebackReg` wakes load (Case B) |
| `RenameHarnessMultiplePendingLoads` | Two loads pending on one store physreg, both woken by single writeback |
| `RenameHarnessLSUCompletionRace` | LSU completes before store writeback; pending entry removed; late writeback does not corrupt LSU value |
| `RenameHarnessSquashCleansPendingAndTables` | Squash rolls back MRT, SRP, and pending-load queue together |
| `RenameHarnessSRPStaticPath` | Push/pop static pairing without MRP training |

## Layer 3: O3 CPU Test Programs

Each subdirectory contains a bare-metal X86 assembly program (`*.S`). The test driver compiles with `gcc -nostdlib`, runs under either stock (`configs/run_o3_stock.py`) or memory renamed (`configs/run_o3_mr.py`) conditions.

Each one of these test programs must:
1. Create `tools/gem5_tests/<name>/<name>.S` with `_start` entry and `exit` syscall
2. Use a loop (≥100 iterations) so the MRP can train past the confidence threshold (default 3)
3. Include self-checks (`cmp` / `jne fail`) so incorrect forwarding returns non-zero
4. Run `./run_tests.sh -v` and inspect `/tmp/gem5_dual_stacks_tests/<name>/` on failure
5. If the test should **not** predict, add the name to the negative list in `check_stats.py`

### Pass criteria

1. Both simulations exit successfully
2. **Stock stats:** `VPsupported`, `VPpredicted`, `VPcorrect` must all be 0
3. **MR stats:** checked by `check_stats.py` (see table below)
4. **Trace equivalence:** `parse_traces.py` strips tick prefixes; cleaned stock and MR exec traces must match (`diff` identical)

### Microbenchmark catalog

| Directory | Purpose | MR stat expectation |
|-----------|---------|---------------------|
| `forwarding_immediate_injection/` | Stack store with delay, then load — Case A immediate value injection | `VPpredicted > 0` |
| `forwarding_delayed_copy/` | `add` → `mov` store (source not ready) → load — Case B pending queue | `VPpredicted > 0` |
| `forwarding_stack_peek/` | `push` + `mov (%rsp)` peek — static SRP path | `VPpredicted > 0` |
| `forwarding_multiple_pending_loads/` | Two loads pending on one not-ready store | `VPpredicted > 0` |
| `no_forwarding_dependency/` | Alternating load-store PC pairing prevents MRP confidence | `VPpredicted == 0` |
| `no_forwarding_size/` | 32-bit store + 64-bit load size mismatch | `VPpredicted == 0` |
| `app_lfsr_memory_pattern/` | Four kernels at store-load distances 1/2/4/50 — dynamic MRP training | `VPpredicted > 0` |
| `app_rpn_calculator/` | RPN stack calculator — repeated push/pop SRP + MRP training | `VPpredicted > 0` |

## Known Gaps & Future Work

1. **Full Rename stage unit test with `DynInst`:** Would require either a lightweight `DynInst` factory or linking `MemoryRenaming` as a `SimObject` in a minimal gem5 test environment. The `RenamePipelineHarness` is the current pragmatic substitute.
2. **IEW / Commit integration:** Squash-after on value misprediction, `bypassWriteback` verification, and `VPcorrect` stat increments at commit are only exercised indirectly via microbenchmark trace equivalence.
3. **Squash-after microbenchmark:** A controlled misprediction test (e.g., deliberately aliased stack memory) would close the remaining Part III gap.
4. **Squash-after recovery** Requires misprediction-inducing microbenchmark

