#include "../analyzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>

#include <mutex>

static void* dummy_mutex_create() {
    return new std::mutex();
}
static void dummy_mutex_destroy(void* mutex) {
    delete static_cast<std::mutex*>(mutex);
}
static void dummy_mutex_lock(void* mutex) {
    static_cast<std::mutex*>(mutex)->lock();
}
static void dummy_mutex_unlock(void* mutex) {
    static_cast<std::mutex*>(mutex)->unlock();
}

// Mock Global State to receive callbacks
static uint32_t g_strict_ipc_push[MAX_TID + 1];
static uint32_t g_strict_ipc_pop[MAX_TID + 1];

static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;

static void dummy_add_strict_ipc(uint8_t tid, uint32_t pushes, uint32_t pops) {
    pthread_mutex_lock(&mock_lock);
    g_strict_ipc_push[tid] += pushes;
    g_strict_ipc_pop[tid] += pops;
    pthread_mutex_unlock(&mock_lock);
}

static void dummy_log_debug(const char *msg) {
    printf("[DEBUG] %s\n", msg);
}

static void reset_mock_state() {
    memset(g_strict_ipc_push, 0, sizeof(g_strict_ipc_push));
    memset(g_strict_ipc_pop, 0, sizeof(g_strict_ipc_pop));
    analyzer_cleanup();
    
    analyzer_callbacks_t cb = {};
    cb.mutex_create = dummy_mutex_create;
    cb.mutex_destroy = dummy_mutex_destroy;
    cb.mutex_lock = dummy_mutex_lock;
    cb.mutex_unlock = dummy_mutex_unlock;
    cb.add_strict_ipc = dummy_add_strict_ipc;
    cb.log_debug = dummy_log_debug;
    analyzer_init(&cb);
}

// Helpers for test validation
static void assert_strict_ipc(uint8_t tid, uint32_t expected_push, uint32_t expected_pop) {
    assert(g_strict_ipc_push[tid] == expected_push);
    assert(g_strict_ipc_pop[tid] == expected_pop);
}

static void assert_pc_ipc(uintptr_t pc, bool is_push, bool expected_is_ipc) {
    bool is_ipc = analyzer_test_is_pc_ipc(pc);
    assert(is_ipc == expected_is_ipc);
}

// ---------------------------------------------------------
// Test Cases
// ---------------------------------------------------------

void test_basic_private() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);
    analyzer_on_ld(1, 0x1000, 8);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    
    assert_strict_ipc(1, 0, 0);
    assert_pc_ipc(0xAA, true, false);
    assert_pc_ipc(0xBB, false, false);
    analyzer_cleanup();
    printf("  Passed!\n");
}

void test_basic_shared() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);
    
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);
    
    analyzer_on_ld(2, 0x1000, 8);
    
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    
    assert_strict_ipc(1, 1, 0);
    assert_strict_ipc(2, 0, 0);
    assert_pc_ipc(0xAA, true, true);
    assert_pc_ipc(0xBB, false, false);
    analyzer_cleanup();
    printf("  Passed!\n");
}

void test_partial_overlap_comprehensive() {
    printf("Running %s...\n", __func__);
    
    struct {
        size_t push_size;
        size_t access_size;
        int access_offset;
        bool expect_sharing;
        const char *desc;
    } cases[] = {
        {8, 4, 0, true, "Exact start, half size"},
        {8, 4, 4, true, "Middle start, half size"},
        {8, 8, 0, true, "Exact match"},
        {8, 2, 2, true, "Inner sliver"},
        {16, 8, 4, true, "Cross-boundary access within allocation"}
    };
    
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset_mock_state();
        
        analyzer_register_thread(1, 0x1000, 0x2000);
        analyzer_register_thread(2, 0x3000, 0x4000);
        
        analyzer_on_push(1, 0x1000, cases[i].push_size, 0xAA, 0);
        analyzer_on_st(1, 0x1000, cases[i].push_size);
        
        uintptr_t access_addr = 0x1000 + cases[i].access_offset;
        analyzer_on_ld(2, access_addr, cases[i].access_size);
        
        analyzer_on_pop(1, 0x1000, cases[i].push_size, 0xBB, 0);
        
        if (cases[i].expect_sharing) {
            assert_strict_ipc(1, 1, 0);
            assert_pc_ipc(0xAA, true, true);
        } else {
            assert_strict_ipc(1, 0, 0);
            assert_pc_ipc(0xAA, true, false);
        }
    }
    analyzer_cleanup();
    printf("  Passed!\n");
}

void test_temporal_depth() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);
    
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_st(1, 0x1000, 8);
    
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    
    analyzer_on_push(1, 0x1000, 8, 0xCC, 0);
    analyzer_on_st(1, 0x1000, 8);
    
    analyzer_on_ld(2, 0x1000, 8);
    
    analyzer_on_pop(1, 0x1000, 8, 0xDD, 0);
    
    assert_strict_ipc(1, 1, 0);
    assert_pc_ipc(0xAA, true, false);
    assert_pc_ipc(0xBB, false, false);
    assert_pc_ipc(0xCC, true, true);
    assert_pc_ipc(0xDD, false, false);
    analyzer_cleanup();
    printf("  Passed!\n");
}

void* worker_thread(void* arg) {
    long tid = (long)arg;
    uintptr_t base = 0x1000 * tid;
    
    for (int i = 0; i < 10000; i++) {
        uintptr_t addr = base + (i % 64) * 8;
        
        analyzer_on_push(tid, addr, 8, 0x1111, 0);
        analyzer_on_st(tid, addr, 8);
        
        if (i % 10 == 0) {
            uint8_t other_tid = (tid == 1) ? 2 : 1;
            uintptr_t other_addr = (0x1000 * other_tid) + ((i/2) % 64) * 8;
            analyzer_on_ld(tid, other_addr, 8);
        }
        
        analyzer_on_pop(tid, addr, 8, 0x2222, 0);
    }
    return NULL;
}

void test_concurrent_stress() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x2000, 0x3000);
    
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker_thread, (void*)1);
    pthread_create(&t2, NULL, worker_thread, (void*)2);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    
    assert(stats.total_push == 20000);
    assert(stats.total_pop == 20000);
    
    assert(g_strict_ipc_push[1] > 0);
    assert(g_strict_ipc_push[2] > 0);
    
    assert_pc_ipc(0x1111, true, true);
    
    analyzer_cleanup();
    printf("  Passed! (Created %llu L2 entries)\n", stats.l2_entries_created);
}

void test_lifetimes() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_add_logical_clock(1, 10);
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 5);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    
    const uint64_t* hist = analyzer_get_histogram();
    assert(hist[5] == 1);
    
    analyzer_add_logical_clock(1, 10);
    analyzer_on_push(1, 0x1008, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 2500); // Exceed max bin
    analyzer_on_pop(1, 0x1008, 8, 0xBB, 0);
    
    hist = analyzer_get_histogram();
    assert(hist[2048] == 1);
    analyzer_cleanup();
    printf("  Passed!\n");
}

void test_hanging_pushes() {
    printf("Running %s...\n", __func__);
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_add_logical_clock(1, 100);
    
    analyzer_drain_hanging_pushes();
    const uint64_t* hist = analyzer_get_histogram();
    assert(hist[100] == 1);
    analyzer_cleanup();
    printf("  Passed!\n");
}


void test_stack_reuse() {
    printf("Running test_stack_reuse...\n");
    reset_mock_state();
    
    analyzer_register_thread(1, 0x1000, 0x2000);
    // Thread 1 pushes
    analyzer_on_push(1, 0x1000, 8, 0xAA, 0);
    analyzer_on_pop(1, 0x1000, 8, 0xBB, 0);
    
    // Thread 1 exits (analyzer_unregister_thread clears tls but not shadow memory)
    analyzer_unregister_thread(1);
    
    // Thread 2 is spawned and OS reuses the same stack address
    analyzer_register_thread(2, 0x1000, 0x2000);
    // Thread 2 pushes to its own stack (which happens to be the same address)
    analyzer_on_push(2, 0x1000, 8, 0xCC, 0);
    analyzer_on_pop(2, 0x1000, 8, 0xDD, 0);
    
    // This should NOT be considered Strict IPC because thread 2 is just reusing the memory for its own stack!
    assert_strict_ipc(2, 0, 0);
    assert_pc_ipc(0xCC, true, false);
    
    analyzer_cleanup();
    printf("  Passed!\n");
}



void test_ghr_context() {
    analyzer_callbacks_t cb = {};
    cb.mutex_create = dummy_mutex_create;
    cb.mutex_destroy = dummy_mutex_destroy;
    cb.mutex_lock = dummy_mutex_lock;
    cb.mutex_unlock = dummy_mutex_unlock;
    cb.add_strict_ipc = dummy_add_strict_ipc;
    cb.log_debug = dummy_log_debug;
    
    analyzer_init(&cb);
    
    analyzer_on_push(1, 0x1000, 8, 0x4000, 2); // PC=0x4000, GHR=2
    analyzer_on_ld(2, 0x1000, 8);              // Shared load (IPC on 0x4000 bucket 2)
    
    analyzer_on_push(1, 0x2000, 8, 0x4000, 3); // PC=0x4000, GHR=3
    analyzer_on_pop(1, 0x2000, 8, 0x5000, 0);  // Private pop
    
    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    
    uint64_t sum_push=0, sum_pop=0, hist_push=0, hist_pop=0;
    uint32_t unique_pcs=0, unique_ipc_pcs=0;
    analyzer_get_pc_stats(&sum_push, &sum_pop, &hist_push, &hist_pop, &unique_pcs, &unique_ipc_pcs, false);
    
    // We should see TWO unique PCs: 0x4000 and 0x5000
    assert(unique_pcs == 2);
    assert(unique_ipc_pcs == 1); // 0x4000 has IPC
    
    // Raw sum for 0x4000 is 2 pushes (bucket 2 and 3)
    assert(sum_push == 2);
    // History-based sum for 0x4000 is 1 push (only bucket 2 is tainted)
    printf("hist_push = %lu\n", hist_push); assert(hist_push == 1);
    
    analyzer_cleanup();
    printf("  Passed!\n");
}


int main() {
    printf("Running test_ghr_context...\n");
    test_ghr_context();

    printf("--- Starting Trace Analyzer Unit Tests ---\n");
    test_stack_reuse();
    test_basic_private();
    test_basic_shared();
    test_partial_overlap_comprehensive();
    test_temporal_depth();
    test_concurrent_stress();
    test_lifetimes();
    test_hanging_pushes();
    printf("--- All Tests Passed! ---\n");
    return 0;
}
