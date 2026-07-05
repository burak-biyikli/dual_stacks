/**
 * Unit tests for the trace analyzer engine (analyzer.cpp).
 *
 * These tests exercise the analyzer's C API directly, without DynamoRIO.
 * The analyzer is decoupled from DynamoRIO via a callback struct, so we
 * provide std::mutex wrappers as the host environment.
 *
 * Test inventory:
 *   Core privacy / IPC detection:
 *     - BasicPrivate:               Single-thread push/st/ld/pop -> no IPC
 *     - BasicShared:                Cross-thread load triggers IPC
 *     - StoreTriggeredIPC:          Cross-thread store triggers IPC
 *     - PartialOverlapComprehensive: 5 sub-cases of partial cross-thread overlap
 *     - TemporalDepth:              Address reuse between push/pop cycles
 *     - StackReuse:                 Thread exit + stack address reuse -> no false IPC
 *
 *   GHR context / history-based metrics:
 *     - GHRContext:                 Same PC, different ctx_hash -> only tainted bucket counted
 *     - HistoryVsNonHistory:        history_sum <= non_history_sum invariant
 *     - MultipleContextBucketTaint: Multiple buckets tainted independently
 *
 *   Lifetime histogram:
 *     - Lifetimes:                  Normal + overflow bin lifetimes
 *     - HangingPushes:              Push without matching pop -> drain records lifetime
 *     - ZeroLifetime:               Push immediately followed by pop at same clock
 *
 *   Concurrency:
 *     - ConcurrentStress:           2 threads x 10K iters with cross-thread reads
 *
 *   Edge cases / boundary conditions:
 *     - MaxTIDBoundary:             TIDs > MAX_TID are silently ignored
 *     - TryLockPcStats:             analyzer_get_pc_stats with try_lock_only=true
 *     - StatsAccumulation:          Verify total ld/st/push/pop counts
 *     - L2EntryLifecycle:           Verify l2_entries_created/drained counts
 *     - PopWithoutPush:             Pop for an address never pushed
 *     - RepeatedCleanup:            Multiple init/cleanup cycles don't crash
 */

#include "../analyzer.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <mutex>
#include <thread>
#include <vector>

// =========================================================
// Mock environment for the analyzer callbacks
// =========================================================

static void* mock_mutex_create() {
    return new std::mutex();
}
static void mock_mutex_destroy(void* mutex) {
    delete static_cast<std::mutex*>(mutex);
}
static void mock_mutex_lock(void* mutex) {
    static_cast<std::mutex*>(mutex)->lock();
}
static bool mock_mutex_trylock(void* mutex) {
    return static_cast<std::mutex*>(mutex)->try_lock();
}
static void mock_mutex_unlock(void* mutex) {
    static_cast<std::mutex*>(mutex)->unlock();
}

// Global mock state to receive strict IPC callbacks
static uint32_t g_strict_ipc_push[MAX_TID + 1];
static uint32_t g_strict_ipc_pop[MAX_TID + 1];
static pthread_mutex_t g_mock_lock = PTHREAD_MUTEX_INITIALIZER;

static void mock_add_strict_ipc(uint8_t tid, uint32_t pushes, uint32_t pops) {
    pthread_mutex_lock(&g_mock_lock);
    g_strict_ipc_push[tid] += pushes;
    g_strict_ipc_pop[tid] += pops;
    pthread_mutex_unlock(&g_mock_lock);
}

static void mock_log_debug(const char *msg) {
    // Suppress debug output during tests unless debugging
    // printf("[DEBUG] %s\n", msg);
}

// =========================================================
// Test fixture
// =========================================================

class AnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(g_strict_ipc_push, 0, sizeof(g_strict_ipc_push));
        memset(g_strict_ipc_pop, 0, sizeof(g_strict_ipc_pop));

        analyzer_callbacks_t cb = {};
        cb.mutex_create  = mock_mutex_create;
        cb.mutex_destroy = mock_mutex_destroy;
        cb.mutex_lock    = mock_mutex_lock;
        cb.mutex_trylock = mock_mutex_trylock;
        cb.mutex_unlock  = mock_mutex_unlock;
        cb.add_strict_ipc = mock_add_strict_ipc;
        cb.log_debug      = mock_log_debug;
        analyzer_init(&cb);
    }

    void TearDown() override {
        analyzer_cleanup();
    }

    // Helpers
    void ExpectStrictIPC(uint8_t tid, uint32_t expected_push, uint32_t expected_pop) {
        EXPECT_EQ(g_strict_ipc_push[tid], expected_push)
            << "Strict IPC push mismatch for tid " << (int)tid;
        EXPECT_EQ(g_strict_ipc_pop[tid], expected_pop)
            << "Strict IPC pop mismatch for tid " << (int)tid;
    }

    void ExpectPcIPC(uintptr_t pc, bool expected_is_ipc) {
        EXPECT_EQ(analyzer_test_is_pc_ipc(pc), expected_is_ipc)
            << "PC IPC mismatch for pc 0x" << std::hex << pc;
    }
};

// =========================================================
// Core privacy / IPC detection tests
// =========================================================

TEST_F(AnalyzerTest, BasicPrivate) {
    analyzer_register_thread(1, 0x1000, 0x2000);

    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);
    analyzer_on_ld(1, 0x1000, 8);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    ExpectStrictIPC(1, 0, 0);
    ExpectPcIPC(0xAA, false);
    ExpectPcIPC(0xBB, false);
}

TEST_F(AnalyzerTest, BasicShared) {
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // Thread 1 pushes and stores
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);

    // Thread 2 loads the same address -> IPC
    analyzer_on_ld(2, 0x1000, 8);

    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    ExpectStrictIPC(1, 1, 0);
    ExpectStrictIPC(2, 0, 0);
    ExpectPcIPC(0xAA, true);   // Push PC is retroactively tainted
    ExpectPcIPC(0xBB, false);  // Pop PC is not tainted (was recorded after drain)
}

TEST_F(AnalyzerTest, StoreTriggeredIPC) {
    // Verify that a cross-thread STORE also triggers IPC detection,
    // not just cross-thread loads.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);

    // Thread 2 STORES to thread 1's pushed address -> IPC
    analyzer_on_st(2, 0x1000, 8);

    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    ExpectStrictIPC(1, 1, 0);
    ExpectPcIPC(0xAA, true);
}

TEST_F(AnalyzerTest, PartialOverlapComprehensive) {
    struct TestCase {
        size_t push_size;
        size_t access_size;
        int access_offset;
        bool expect_sharing;
        const char *desc;
    };

    TestCase cases[] = {
        {8, 4, 0, true, "Exact start, half size"},
        {8, 4, 4, true, "Middle start, half size"},
        {8, 8, 0, true, "Exact match"},
        {8, 2, 2, true, "Inner sliver"},
        {16, 8, 4, true, "Cross-boundary access within allocation"},
    };

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.desc);

        // Reset state for each sub-case
        TearDown();
        SetUp();

        analyzer_register_thread(1, 0x1000, 0x2000);
        analyzer_register_thread(2, 0x3000, 0x4000);

        analyzer_on_push(1, 0x1000, tc.push_size, 0xAA, 0);
        analyzer_on_st(1, 0x1000, tc.push_size);

        uintptr_t access_addr = 0x1000 + tc.access_offset;
        analyzer_on_ld(2, access_addr, tc.access_size);

        analyzer_on_pop(1, 0x1000, tc.push_size, 0xBB, 0);

        if (tc.expect_sharing) {
            ExpectStrictIPC(1, 1, 0);
            ExpectPcIPC(0xAA, true);
        } else {
            ExpectStrictIPC(1, 0, 0);
            ExpectPcIPC(0xAA, false);
        }
    }
}

TEST_F(AnalyzerTest, TemporalDepth) {
    // Verify that only the CURRENT push/pop cycle is flagged,
    // not a previous cycle at the same address.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // Cycle 1: push/pop at 0x1000 (private)
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    // Cycle 2: push at same address, then cross-thread read
    analyzer_on_push(1, 0x1000, 8, 0xCC, 0);
    analyzer_on_st(1, 0x1000, 8);
    analyzer_on_ld(2, 0x1000, 8);
    analyzer_on_pop(1, 0x1000, 8, 0xDD, 0);

    ExpectStrictIPC(1, 1, 0);
    ExpectPcIPC(0xAA, false);  // Cycle 1 push PC: private
    ExpectPcIPC(0xBB, false);  // Cycle 1 pop PC: private
    ExpectPcIPC(0xCC, true);   // Cycle 2 push PC: tainted
    ExpectPcIPC(0xDD, false);  // Cycle 2 pop PC: not tainted
}

TEST_F(AnalyzerTest, StackReuse) {
    // When a thread exits and a new thread reuses the same stack address,
    // it should NOT trigger false IPC.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    analyzer_unregister_thread(1);

    // New thread reuses the same stack address range
    analyzer_register_thread(2, 0x1000, 0x2000);
    analyzer_on_push(2, 0x1000, 8, 0xCC, 0);
    analyzer_on_pop(2, 0x1000, 8, 0xDD, 0);

    // Should NOT be IPC — thread 2 owns this address via its own push
    ExpectStrictIPC(2, 0, 0);
    ExpectPcIPC(0xCC, false);
}

// =========================================================
// GHR context / history-based metric tests
// =========================================================

TEST_F(AnalyzerTest, GHRContext) {
    // Same PC (0x4000) called with two different ctx_hash values.
    // Only ctx_hash=2 is tainted by IPC. Verify that the history-based
    // metric counts only the tainted bucket.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // Push with PC=0x4000, ctx_hash=2 -> this will be tainted
    analyzer_on_push(1, 0x1000, 8, 0x4000, 2);
    analyzer_on_ld(2, 0x1000, 8);  // Cross-thread read -> IPC

    // Push with PC=0x4000, ctx_hash=3 -> this stays private
    analyzer_on_push(1, 0x2000, 8, 0x4000, 3);
    analyzer_on_pop(1, 0x2000, 8, 0x5000, 0);

    uint64_t sum_push = 0, sum_pop = 0, hist_push = 0, hist_pop = 0;
    uint32_t unique_pcs = 0, unique_ipc_pcs = 0;
    analyzer_get_pc_stats(&sum_push, &sum_pop, &hist_push, &hist_pop,
                          &unique_pcs, &unique_ipc_pcs, false);

    // Two unique PCs: 0x4000 (push) and 0x5000 (pop)
    EXPECT_EQ(unique_pcs, 2u);
    // Only 0x4000 has IPC
    EXPECT_EQ(unique_ipc_pcs, 1u);
    // Non-history: ALL instances of PC 0x4000 counted (2 pushes)
    EXPECT_EQ(sum_push, 2u);
    // History-based: only ctx_hash=2 bucket counted (1 push)
    EXPECT_EQ(hist_push, 1u);
}

TEST_F(AnalyzerTest, HistoryVsNonHistory) {
    // The history-based count should always be <= the non-history count.
    // Set up 4 context buckets for PC 0x4000, taint only 1.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // 4 pushes with different ctx_hash values at different addresses
    uintptr_t addrs[] = {0x1000, 0x1100, 0x1200, 0x1300};
    for (int i = 0; i < 4; i++) {
        analyzer_on_push(1, addrs[i], 8, 0x4000, i);
        analyzer_on_pop(1, addrs[i], 8, 0x5000, 0);
    }

    // Taint only ctx_hash=0 by doing a push + cross-thread read
    analyzer_on_push(1, 0x1400, 8, 0x4000, 0);
    analyzer_on_ld(2, 0x1400, 8);

    uint64_t sum_push, sum_pop, hist_push, hist_pop;
    uint32_t unique_pcs, unique_ipc_pcs;
    analyzer_get_pc_stats(&sum_push, &sum_pop, &hist_push, &hist_pop,
                          &unique_pcs, &unique_ipc_pcs, false);

    // Non-history counts ALL 5 pushes for the IPC-tainted PC
    EXPECT_EQ(sum_push, 5u);
    // History counts only pushes in tainted ctx_hash=0 bucket: that's 2 (one from the loop + the taint push)
    EXPECT_EQ(hist_push, 2u);
    // Invariant: history <= non-history
    EXPECT_LE(hist_push, sum_push);
}

TEST_F(AnalyzerTest, MultipleContextBucketTaint) {
    // Taint two different context buckets for the same PC independently.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // Push with ctx_hash=1, taint it
    analyzer_on_push(1, 0x1000, 8, 0x4000, 1);
    analyzer_on_ld(2, 0x1000, 8);

    // Push with ctx_hash=5, taint it
    analyzer_on_push(1, 0x1100, 8, 0x4000, 5);
    analyzer_on_ld(2, 0x1100, 8);

    // Push with ctx_hash=3, keep private
    analyzer_on_push(1, 0x1200, 8, 0x4000, 3);
    analyzer_on_pop(1, 0x1200, 8, 0x5000, 0);

    uint64_t sum_push, sum_pop, hist_push, hist_pop;
    uint32_t unique_pcs, unique_ipc_pcs;
    analyzer_get_pc_stats(&sum_push, &sum_pop, &hist_push, &hist_pop,
                          &unique_pcs, &unique_ipc_pcs, false);

    EXPECT_EQ(unique_ipc_pcs, 1u);
    // Non-history: all 3 pushes for tainted PC
    EXPECT_EQ(sum_push, 3u);
    // History: buckets 1 and 5 are tainted (1 push each)
    EXPECT_EQ(hist_push, 2u);
}

// =========================================================
// Lifetime histogram tests
// =========================================================

TEST_F(AnalyzerTest, Lifetimes) {
    analyzer_register_thread(1, 0x1000, 0x2000);

    // Short lifetime: 5 ticks
    analyzer_add_logical_clock(1, 10);
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 5);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    const uint64_t* hist = analyzer_get_histogram();
    EXPECT_EQ(hist[5], 1u);

    // Long lifetime: 2500 ticks -> overflow bin (2048)
    analyzer_add_logical_clock(1, 10);
    analyzer_on_push(1, 0x1008, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 2500);
    analyzer_on_pop(1, 0x1008, 8, 0xBB, 0);

    hist = analyzer_get_histogram();
    EXPECT_EQ(hist[2048], 1u);
}

TEST_F(AnalyzerTest, ZeroLifetime) {
    // Push and pop at the same logical clock -> lifetime = 0
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_add_logical_clock(1, 50);

    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    // No clock advance between push and pop
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    const uint64_t* hist = analyzer_get_histogram();
    EXPECT_EQ(hist[0], 1u);
}

TEST_F(AnalyzerTest, HangingPushes) {
    // A push that never gets a matching pop. The lifetime should be
    // recorded when analyzer_drain_hanging_pushes() is called.
    analyzer_register_thread(1, 0x1000, 0x2000);

    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 100);

    analyzer_drain_hanging_pushes();

    const uint64_t* hist = analyzer_get_histogram();
    EXPECT_EQ(hist[100], 1u);
}

// =========================================================
// Concurrency tests
// =========================================================

static void* stress_worker(void* arg) {
    long tid = (long)arg;
    uintptr_t base = 0x1000 * tid;

    for (int i = 0; i < 10000; i++) {
        uintptr_t addr = base + (i % 64) * 8;

        analyzer_on_push(tid, addr, 8, 0x1111, 0);
        analyzer_on_st(tid, addr, 8);

        // Every 10th iteration, read from the other thread's region
        if (i % 10 == 0) {
            uint8_t other_tid = (tid == 1) ? 2 : 1;
            uintptr_t other_addr = (0x1000 * other_tid) + ((i / 2) % 64) * 8;
            analyzer_on_ld(tid, other_addr, 8);
        }

        analyzer_on_pop(tid, addr, 8, 0x2222, 0);
    }
    return NULL;
}

TEST_F(AnalyzerTest, ConcurrentStress) {
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x2000, 0x3000);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, stress_worker, (void*)1);
    pthread_create(&t2, NULL, stress_worker, (void*)2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    analyzer_stats_t stats;
    analyzer_get_stats(&stats);

    EXPECT_EQ(stats.total_push, 20000u);
    EXPECT_EQ(stats.total_pop, 20000u);

    // Both threads should have had some IPC detected
    EXPECT_GT(g_strict_ipc_push[1], 0u);
    EXPECT_GT(g_strict_ipc_push[2], 0u);

    ExpectPcIPC(0x1111, true);
}

// =========================================================
// Edge cases / boundary conditions
// =========================================================

TEST_F(AnalyzerTest, MaxTIDBoundary) {
    // TIDs > MAX_TID should be silently ignored without crashes.
    uint8_t over_tid = MAX_TID + 1;

    // These should all be no-ops, not crash
    analyzer_register_thread(over_tid, 0x1000, 0x2000);
    analyzer_add_logical_clock(over_tid, 100);
    analyzer_unregister_thread(over_tid);

    // Verify normal TID still works fine after the above
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    ExpectStrictIPC(1, 0, 0);
}

TEST_F(AnalyzerTest, TryLockPcStats) {
    // Verify analyzer_get_pc_stats with try_lock_only=true returns data.
    // Under no contention, try_lock should succeed for all banks.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_ld(2, 0x1000, 8);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);

    uint64_t sum_push, sum_pop, hist_push, hist_pop;
    uint32_t unique_pcs, unique_ipc_pcs;

    // try_lock_only=true
    analyzer_get_pc_stats(&sum_push, &sum_pop, &hist_push, &hist_pop,
                          &unique_pcs, &unique_ipc_pcs, true);

    // Under no contention, should get same results as normal mode
    EXPECT_GE(unique_pcs, 1u);
    EXPECT_GE(unique_ipc_pcs, 1u);
}

TEST_F(AnalyzerTest, StatsAccumulation) {
    // Verify that total ld/st/push/pop stats are correctly accumulated.
    analyzer_register_thread(1, 0x1000, 0x2000);

    const int N = 50;
    for (int i = 0; i < N; i++) {
        analyzer_on_push(1, 0x1000 + i * 8, 8, 0xAA, 0);
        analyzer_on_st(1, 0x1000 + i * 8, 8);
        analyzer_on_ld(1, 0x1000 + i * 8, 8);
        analyzer_on_pop(1, 0x1000 + i * 8, 8, 0xBB, 0);
    }

    analyzer_stats_t stats;
    analyzer_get_stats(&stats);

    EXPECT_EQ(stats.total_push, (uint64_t)N);
    EXPECT_EQ(stats.total_pop, (uint64_t)N);
    EXPECT_EQ(stats.total_st, (uint64_t)N);
    EXPECT_EQ(stats.total_ld, (uint64_t)N);
}

TEST_F(AnalyzerTest, L2EntryLifecycle) {
    // Verify L2 entry creation and draining counts.
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);

    // Create L2 entries via pushes
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_push(1, 0x1008, 8, 0xBB, 0);

    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    EXPECT_GE(stats.l2_entries_created, 2u);

    // Trigger IPC drain on one entry
    analyzer_on_ld(2, 0x1000, 8);
    analyzer_get_stats(&stats);
    EXPECT_GE(stats.l2_entries_drained, 1u);
}

TEST_F(AnalyzerTest, PopWithoutPush) {
    // A pop for an address that was never pushed should not crash.
    // It should still be counted in stats.
    analyzer_register_thread(1, 0x1000, 0x2000);

    analyzer_on_pop(1, 0x5000, 8, 0xAA, 0);

    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    EXPECT_EQ(stats.total_pop, 1u);
}

TEST_F(AnalyzerTest, RepeatedCleanup) {
    // Multiple init/cleanup cycles should not crash or leak.
    for (int cycle = 0; cycle < 3; cycle++) {
        analyzer_cleanup();

        analyzer_callbacks_t cb = {};
        cb.mutex_create  = mock_mutex_create;
        cb.mutex_destroy = mock_mutex_destroy;
        cb.mutex_lock    = mock_mutex_lock;
        cb.mutex_trylock = mock_mutex_trylock;
        cb.mutex_unlock  = mock_mutex_unlock;
        cb.add_strict_ipc = mock_add_strict_ipc;
        cb.log_debug      = mock_log_debug;
        analyzer_init(&cb);

        analyzer_register_thread(1, 0x1000, 0x2000);
        analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
        analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    }

    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    // Only the last cycle's stats should remain
    EXPECT_EQ(stats.total_push, 1u);
    EXPECT_EQ(stats.total_pop, 1u);
}
