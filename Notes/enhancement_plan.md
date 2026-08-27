================================================================================
MEMORY RENAMING BUG FIX AND TEST EXPANSION PLAN
================================================================================

1. SUMMARY
This plan addresses two critical bugs identified during the simulation of the 
parsec_vips workloads: a severe O(N) performance bottleneck caused by an 
SRP-Stack memory leak, and architectural state corruption stemming from 
improper vector instruction forwarding. Additionally, we will expand the 
hybrid testing infrastructure in run_tests.sh to include vips and expand 
unit tests to cover vector exclusion and stack classification.

2. ISSUE SYMPTOMS
* Performance Drop: Workloads heavily utilizing local stack variables 
  (like parsec_vips) cannot pass 2.7M instructions as shown in 
  `2026-07-12_18-32-52_MRP_SMALL/` and 
  `2026-07-12_01-11-54_MRP_TEST/` where no MR run passed 1.1M and 
  2.7M instructions respectively.
* Infinite Loops / Corruption: Vector loads incorrectly inherit the base 
  address register (e.g., rsp) instead of actual vector data, bypassing 
  misprediction checks and corrupting architectural state.

3. TEST EXPANSION FOR VIPS
* run_tests.sh modifications (line 209): Add 'parsec_vips' to the macrobenchmarks 
  array alongside "bfs" and "blackscholes". The binary path follows the PARSEC pattern:
  `ext/parsec-benchmark/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips`
* Unit Tests: Expand memory_renaming_rename_pipeline.test.cc to include 
  negative tests ensuring vector loads and stores are ignored by the 
  predictor. Expand memory_renaming.test.cc to test SRP-Stack pushes 
  using standard stack stores (verifying they do not leak).

4. SRP STACK CHANGES
* File: ext/gem5/src/cpu/valuepred/memory_renaming.cc
* Change: In renameStore (lines 92-100), modify the SRP-Stack push condition. 
  Currently the code checks `if (stackInfo.isStackBase)` and pushes ALL stack stores 
  (including standard local variable stores) to the SRP-Stack. This causes the 
  LIFO stack to accumulate entries indefinitely for workloads with frequent local 
  stack usage. Update this to `if (stackInfo.isStackBase && stackInfo.isPush)` 
  so that only actual push/call instructions are pushed to the SRP-Stack, while 
  standard stack stores follow the dynamic path.

5. VECTOR FORWARDING CHANGES
* File: ext/gem5/src/cpu/o3/rename.cc
* Change: In renameInsts (lines 754-775), add vector instruction filtering. 
  For stores: Add `&& !inst->isVector()` to the store identification block 
  (line 757) to prevent vector stores from allocating MRT entries and being 
  pushed to the SRP-Stack. Vector loads are already filtered by `canLVP()` 
  in the else-if branch (line 769), but the store branch needs explicit 
  vector exclusion to avoid corrupting architectural state with invalid data.
  The fix changes:
  ```cpp
  if (inst->isStore()) {
  ```
  to:
  ```cpp
  if (inst->isStore() && !inst->isVector()) {
  ```

6. PENDING LOADS CHANGE
* File: ext/gem5/src/cpu/valuepred/memory_renaming.cc
* Change: In MemoryRenaming::squash (lines 216-229), after iterating through 
  pendingLoads and removing loads where `seqNum > seq_no`, add logic to check 
  if each list is empty and remove empty map keys. Also update the corresponding 
  RenamePipelineHarness::squash method in memory_renaming_pipeline_harness.hh 
  (lines 197-209) to use the same cleanup pattern for test consistency.
  Add:
  ```cpp
  for (auto it = pendingLoads.begin(); it != pendingLoads.end(); ) {
      if (it->second.empty()) {
          it = pendingLoads.erase(it);
      } else {
          ++it;
      }
  }
  ```
  after the remove_if to prevent hash map bloating over long-running workloads.

7. VERIFICATION STEPS
* Build the updated gem5 and run all unit tests: 
  `scons build/X86/cpu/valuepred/memory_renaming.test.opt build/X86/cpu/valuepred/memory_renaming_rename_pipeline.test.opt build/X86/cpu/valuepred/memory_renaming_commit_pipeline.test.opt`
* Execute `run_tests.sh -v` to verify trace parity in Phase 1 before macrobenchmarks.
* Run the expanded hybrid loop with parsec_vips added and verify 15M instruction 
  completion in Stress Mode.
* Run `05_run_simulations.py` and verify parsec_vips_small and parsec_vips_test 
  successfully reach at least 3M instructions within the 600-second timeout.
  * If trace differences occur, verify `trace_stock.txt` and `trace_mr.txt` for 
    vips match exactly up to the instruction count reached.

8. TASK CHECKLIST
[ ] Modify renameStore in memory_renaming.cc (SRP-Stack leak fix)
[ ] Modify renameInsts in rename.cc (Vector restriction fix)
[ ] Modify squash in memory_renaming.cc (Empty map key cleanup for pendingLoads)
[ ] Add parsec_vips to the macrobenchmarks array in run_tests.sh
[ ] Write unit tests for vector exclusions in the rename pipeline harness
[ ] Write unit tests for standard stack store handling in the core unit tests
[ ] Run run_tests.sh -v to ensure trace parity
[ ] Run 05_run_simulations.py to verify 50M instruction completion
================================================================================