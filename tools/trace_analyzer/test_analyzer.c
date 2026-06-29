#include "analyzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>

// Mock Global State to receive callbacks
static uint32_t g_strict_ipc_push[MAX_TID + 1];
static uint32_t g_strict_ipc_pop[MAX_TID + 1];

typedef struct {
    uintptr_t pc;
    uint32_t total_count;
    bool is_ipc;
    bool is_push;
} mock_pc_t;

static mock_pc_t g_pcs[100];
static int g_num_pcs = 0;
static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;

static mock_pc_t* get_pc(uintptr_t pc, bool is_push) {
    for (int i = 0; i < g_num_pcs; i++) {
        if (g_pcs[i].pc == pc && g_pcs[i].is_push == is_push) {
            return &g_pcs[i];
        }
    }
    g_pcs[g_num_pcs].pc = pc;
    g_pcs[g_num_pcs].is_push = is_push;
    g_pcs[g_num_pcs].total_count = 0;
    g_pcs[g_num_pcs].is_ipc = false;
    return &g_pcs[g_num_pcs++];
}

static void cb_mark_pc_ipc(uintptr_t pc, bool is_push) {
    pthread_mutex_lock(&mock_lock);
    mock_pc_t *p = get_pc(pc, is_push);
    p->is_ipc = true;
    pthread_mutex_unlock(&mock_lock);
}

static void cb_add_strict_ipc(uint8_t tid, uint32_t pushes, uint32_t pops) {
    pthread_mutex_lock(&mock_lock);
    g_strict_ipc_push[tid] += pushes;
    g_strict_ipc_pop[tid] += pops;
    pthread_mutex_unlock(&mock_lock);
}

static void cb_log_debug(const char *msg) {
    printf("[DEBUG] %s\n", msg);
}

static void* cb_mutex_create(void) {
    pthread_mutex_t *mut = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mut, NULL);
    return mut;
}

static void cb_mutex_lock(void *mutex) {
    pthread_mutex_lock((pthread_mutex_t*)mutex);
}

static void cb_mutex_unlock(void *mutex) {
    pthread_mutex_unlock((pthread_mutex_t*)mutex);
}

static void cb_mutex_destroy(void *mutex) {
    pthread_mutex_destroy((pthread_mutex_t*)mutex);
    free(mutex);
}

static void reset_mock_state() {
    memset(g_strict_ipc_push, 0, sizeof(g_strict_ipc_push));
    memset(g_strict_ipc_pop, 0, sizeof(g_strict_ipc_pop));
    memset(g_pcs, 0, sizeof(g_pcs));
    g_num_pcs = 0;
    analyzer_cleanup();
    
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc,
        .log_debug = cb_log_debug,
        .mutex_create = cb_mutex_create,
        .mutex_lock = cb_mutex_lock,
        .mutex_unlock = cb_mutex_unlock,
        .mutex_destroy = cb_mutex_destroy
    };
    analyzer_init(cb);
}

// Helpers for test validation
static void assert_strict_ipc(uint8_t tid, uint32_t expected_push, uint32_t expected_pop) {
    assert(g_strict_ipc_push[tid] == expected_push);
    assert(g_strict_ipc_pop[tid] == expected_pop);
}

static void assert_pc_ipc(uintptr_t pc, bool is_push, bool expected_is_ipc) {
    mock_pc_t *p = get_pc(pc, is_push);
    assert(p->is_ipc == expected_is_ipc);
}


// --- Test Cases ---

/*
 * test_basic_private
 * Tests that a simple PUSH and POP on a thread's own stack is correctly
 * identified as purely private with no IPC overhead.
 */
static void test_basic_private() {
    reset_mock_state();
    analyzer_register_thread(1, 0x1000, 0x2000);
    
    // Thread 1 pushes and pops on its own stack
    analyzer_on_push(1, 0x1500, 8, 0x4000); // pc 0x4000
    analyzer_on_pop (1, 0x1500, 8, 0x4008); // pc 0x4008
    
    assert_strict_ipc(1, 0, 0);
    assert_pc_ipc(0x4000, true, false);
    assert_pc_ipc(0x4008, false, false);
    printf("PASS: test_basic_private\n");
}

/*
 * test_true_sharing_drain
 * Tests that if Thread 1 pushes data to its stack, and Thread 2 reads it,
 * the push is retroactively marked as Strict IPC and drained from the L2 table.
 */
static void test_true_sharing_drain() {
    reset_mock_state();
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x2000, 0x3000);
    
    // Thread 1 pushes
    analyzer_on_push(1, 0x1500, 8, 0x4000);
    
    // Thread 2 reads from Thread 1's stack
    analyzer_on_ld(2, 0x1500, 8);
    
    // Thread 1's push should be retroactively drained as IPC
    assert_strict_ipc(1, 1, 0);
    assert_pc_ipc(0x4000, true, true);
    printf("PASS: test_true_sharing_drain\n");
}

/*
 * test_no_false_sharing
 * Tests that if Thread 1 and Thread 2 push to adjacent memory addresses
 * (simulating independent stacks that happen to be close or adjacent),
 * no false sharing is triggered.
 */
static void test_no_false_sharing() {
    reset_mock_state();
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x2000, 0x3000);
    
    // Thread 1 pushes to 0x1500 (8 bytes)
    analyzer_on_push(1, 0x1500, 8, 0x4000);
    
    // Thread 2 pushes to 0x1508 (adjacent 8 bytes, same CL on most archs)
    analyzer_on_push(2, 0x1508, 8, 0x4000);
    
    // No sharing!
    assert_strict_ipc(1, 0, 0);
    assert_strict_ipc(2, 0, 0);
    printf("PASS: test_no_false_sharing\n");
}

/*
 * test_stack_reuse_allocate_on_push
 * Tests temporal reuse of stack memory. If memory was previously marked as IPC,
 * a subsequent PUSH to that exact same memory address (by the owner thread)
 * correctly resets the lifetime and marks the new data as private.
 */
static void test_stack_reuse_allocate_on_push() {
    reset_mock_state();
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x2000, 0x3000);
    
    // Thread 1 pushes, Thread 2 reads -> IPC
    analyzer_on_push(1, 0x1500, 8, 0x4000);
    analyzer_on_ld(2, 0x1500, 8);
    assert_strict_ipc(1, 1, 0);
    
    // Later, Thread 1's stack unwinds and reuses 0x1500
    // The PUSH should wipe the SHARED state
    analyzer_on_push(1, 0x1500, 8, 0x5000); // Different PC
    
    // Total strict IPC pushes should still be 1! (The new push is private)
    assert_strict_ipc(1, 1, 0);
    assert_pc_ipc(0x5000, true, false); // New PC is NOT ipc
    
    // A subsequent POP should also be private
    analyzer_on_pop(1, 0x1500, 8, 0x5008);
    assert_strict_ipc(1, 1, 0); // No pops were IPC
    
    printf("PASS: test_stack_reuse_allocate_on_push\n");
}

/*
 * test_partial_overlap_comprehensive
 * Tests the O(1) offset shadow directory logic against all possible 
 * unaligned accesses (0 to 8 bytes offset) to mathematically prove it
 * catches overlapping IPC perfectly without false sharing.
 */
static void test_partial_overlap_comprehensive() {
    printf("Running test_partial_overlap_comprehensive...\n");
    analyzer_cleanup();
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc,
        .log_debug = cb_log_debug,
        .mutex_create = cb_mutex_create,
        .mutex_lock = cb_mutex_lock,
        .mutex_unlock = cb_mutex_unlock,
        .mutex_destroy = cb_mutex_destroy
    };
    analyzer_init(cb);
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);
    
    // We will test 8-byte PUSH at various base addresses,
    // and a 4-byte read at all overlapping offsets (-4 to +8).
    
    for (int align_offset = 0; align_offset < 8; align_offset++) {
        for (int read_offset = -4; read_offset <= 8; read_offset++) {
            analyzer_cleanup();
            analyzer_init(cb);
            analyzer_register_thread(1, 0x1000, 0x2000);
            analyzer_register_thread(2, 0x3000, 0x4000);
            reset_mock_state();
            
            uintptr_t push_addr = 0x1500 + align_offset;
            uintptr_t read_addr = push_addr + read_offset;
            
            // Thread 1 pushes 8 bytes
            analyzer_on_push(1, push_addr, 8, 0xAA);
            
            // Thread 2 reads 4 bytes
            analyzer_on_ld(2, read_addr, 4);
            
            // Calculate overlap
            long overlap_start = (push_addr > read_addr) ? push_addr : read_addr;
            long overlap_end = ((push_addr + 8) < (read_addr + 4)) ? (push_addr + 8) : (read_addr + 4);
            bool has_overlap = overlap_start < overlap_end;
            
            int strict_pushes = g_strict_ipc_push[1];
            int ipc_pc_count = 0;
            for (int i = 0; i < g_num_pcs; i++) {
                if (g_pcs[i].is_ipc) ipc_pc_count++;
            }
            
            if (has_overlap) {
                if (strict_pushes != 1) {
                    printf("FAIL: align_offset=%d, read_offset=%d. Expected 1 strict push, got %d\n", align_offset, read_offset, strict_pushes);
                    assert(strict_pushes == 1);
                }
                if (ipc_pc_count != 1) {
                    printf("FAIL: align_offset=%d, read_offset=%d. Expected 1 IPC PC, got %d\n", align_offset, read_offset, ipc_pc_count);
                    assert(ipc_pc_count == 1);
                }
            } else {
                if (strict_pushes != 0) {
                    printf("FAIL: align_offset=%d, read_offset=%d. Expected 0 strict push, got %d\n", align_offset, read_offset, strict_pushes);
                    assert(strict_pushes == 0);
                }
                if (ipc_pc_count != 0) {
                    printf("FAIL: align_offset=%d, read_offset=%d. Expected 0 IPC PC, got %d\n", align_offset, read_offset, ipc_pc_count);
                    assert(ipc_pc_count == 0);
                }
            }
        }
    }
    
    // Also test the deletion bug: 
    // Two independent pushes mapping to same chunk, then one is popped.
    analyzer_cleanup();
    analyzer_init(cb);
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);
    reset_mock_state();
    
    analyzer_on_push(1, 0x149C, 2, 0xAA);
    analyzer_on_push(1, 0x149E, 2, 0xBB);
    
    // Pop the first one
    analyzer_on_pop(1, 0x149C, 2, 0xCC);
    
    // Thread 2 reads the second one
    analyzer_on_ld(2, 0x149E, 2);
    
    int strict_pushes = g_strict_ipc_push[1];
    // We expect the second one to still be drained and flag strict IPC!
    if (strict_pushes != 1) {
        printf("FAIL: Deletion bug! Expected 1 strict push, got %d\n", strict_pushes);
        assert(strict_pushes == 1);
    }
    
    printf("test_partial_overlap_comprehensive passed!\n");
}


static void test_temporal_depth() {
    printf("Running test_temporal_depth...\n");
    analyzer_cleanup();
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc,
        .log_debug = cb_log_debug,
        .mutex_create = cb_mutex_create,
        .mutex_lock = cb_mutex_lock,
        .mutex_unlock = cb_mutex_unlock,
        .mutex_destroy = cb_mutex_destroy
    };
    analyzer_init(cb);
    analyzer_register_thread(1, 0x1000, 0x2000);
    analyzer_register_thread(2, 0x3000, 0x4000);
    
    // Test depth 4 call stack, simulating patterns of private vs IPC
    // We will do a sequence of pushes (call depth), then pops, across iterations.
    for (int iter = 0; iter < 10; iter++) {
        reset_mock_state();
        
        // Push 4 frames
        for (int depth = 0; depth < 4; depth++) {
            analyzer_on_push(1, 0x1500 + (depth * 8), 8, 0x100 + depth);
        }
        
        // Some iterations have IPC, some don't based on bitmask of iteration
        int expected_ipc_pushes = 0;
        for (int depth = 0; depth < 4; depth++) {
            if (iter & (1 << depth)) {
                // Thread 2 reads this depth's memory
                analyzer_on_ld(2, 0x1500 + (depth * 8) + 2, 4);
                expected_ipc_pushes++;
            }
        }
        
        assert(g_strict_ipc_push[1] == expected_ipc_pushes);
        
        // Pop the 4 frames
        for (int depth = 3; depth >= 0; depth--) {
            analyzer_on_pop(1, 0x1500 + (depth * 8), 8, 0x200 + depth);
        }
        
        // Ensure no extra IPC popped up
        assert(g_strict_ipc_push[1] == expected_ipc_pushes);
    }
    
    printf("test_temporal_depth passed!\n");
}

static void* stress_worker(void* arg) {
    uintptr_t thread_id = (uintptr_t)arg;
    uintptr_t my_stack = 0x10000 + (thread_id * 0x1000);
    
    // Each thread pushes, pops, loads, stores in its own area,
    // and sometimes reads from thread 0's stack to cause IPC!
    for (int i = 0; i < 5000; i++) {
        uintptr_t addr = my_stack + (i % 100) * 8;
        
        // Push
        analyzer_on_push(thread_id, addr, 8, 0xAA00 + thread_id);
        
        // Randomly do a read from thread 0
        if (thread_id != 0 && (i % 10) == 0) {
            analyzer_on_ld(thread_id, 0x10000 + (i % 100) * 8, 8);
        }
        
        // Pop
        analyzer_on_pop(thread_id, addr, 8, 0xBB00 + thread_id);
    }
    return NULL;
}

static void test_concurrent_stress() {
    printf("Running test_concurrent_stress...\n");
    analyzer_cleanup();
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc,
        .log_debug = cb_log_debug,
        .mutex_create = cb_mutex_create,
        .mutex_lock = cb_mutex_lock,
        .mutex_unlock = cb_mutex_unlock,
        .mutex_destroy = cb_mutex_destroy
    };
    analyzer_init(cb);
    
    // Register threads
    for (int i = 0; i < 8; i++) {
        analyzer_register_thread(i, 0x10000 + (i * 0x1000), 0x10000 + ((i+1) * 0x1000));
    }
    
    reset_mock_state();
    
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, stress_worker, (void*)(uintptr_t)i);
    }
    
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // The results will be somewhat non-deterministic in exact count due to race of reading vs pushing 
    // on thread 0's stack, but we can verify it doesn't crash and we catch IPC correctly.
    // Thread 0 should have a non-zero strict IPC push count.
    pthread_mutex_lock(&mock_lock);
    int t0_ipc = g_strict_ipc_push[0];
    pthread_mutex_unlock(&mock_lock);
    
    assert(t0_ipc > 0);
    
    printf("test_concurrent_stress passed with %d IPC instances caught!\n", t0_ipc);
}

static void test_lifetimes() {
    printf("Running test_lifetimes...\n");
    analyzer_cleanup();
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc,
        .log_debug = cb_log_debug,
        .mutex_create = cb_mutex_create,
        .mutex_lock = cb_mutex_lock,
        .mutex_unlock = cb_mutex_unlock,
        .mutex_destroy = cb_mutex_destroy
    };
    analyzer_init(cb);
    analyzer_register_thread(1, 0x1000, 0x2000);
    
    // Initial clock is 0
    analyzer_on_push(1, 0x1500, 8, 0x100);
    
    // Update clock by 5 ticks
    analyzer_update_clock(1, 5);
    analyzer_on_push(1, 0x14F8, 8, 0x101);
    
    // Update clock by 100 ticks
    analyzer_update_clock(1, 100);
    analyzer_on_push(1, 0x14F0, 8, 0x102);
    
    // Update clock by 2500 ticks (over max bin)
    analyzer_update_clock(1, 2500);
    
    // Pop them in reverse order
    // 0x14F0 pushed at 105, popped at 2605. Lifetime = 2500 (capped at 2048)
    analyzer_on_pop(1, 0x14F0, 8, 0x202);
    
    // 0x14F8 pushed at 5, popped at 2605. Lifetime = 2600 (capped at 2048)
    analyzer_on_pop(1, 0x14F8, 8, 0x201);
    
    // Update clock by 10
    analyzer_update_clock(1, 10);
    // 0x1500 pushed at 0, popped at 2615. Lifetime = 2615 (capped at 2048)
    analyzer_on_pop(1, 0x1500, 8, 0x200);
    
    // Let's do a short one
    analyzer_on_push(1, 0x1500, 8, 0x100); // pushed at 2615
    analyzer_update_clock(1, 42);
    analyzer_on_pop(1, 0x1500, 8, 0x200); // popped at 2657. Lifetime = 42
    
    const uint64_t* histo = analyzer_get_histogram();
    assert(histo[42] == 1);
    assert(histo[2048] == 3);
    
    printf("test_lifetimes passed!\n");
}

int main() {
    printf("Running trace_analyzer synthetic tests...\n");
    
    test_basic_private();
    test_true_sharing_drain();
    test_no_false_sharing();
    test_stack_reuse_allocate_on_push();
    test_partial_overlap_comprehensive();
    test_temporal_depth();
    test_concurrent_stress();
    test_lifetimes();
    
    printf("All tests PASSED!\n");
    return 0;
}
