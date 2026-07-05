/**
 * Integration stress test for the trace analyzer.
 *
 * This program is designed to be run UNDER DynamoRIO with the analyzer client:
 *   drrun -c libanalyzer.so -- ./test_fuzzer
 *
 * It exercises the analyzer's real instrumentation pipeline (not the C API directly)
 * with a multi-threaded workload that creates both private and IPC stack operations.
 *
 * Test scenario:
 *   - Main thread allocates a stack variable ("anchor") and exposes it to workers
 *   - Workers churn through deep recursive calls (private stack work)
 *   - Workers read the anchor (IPC) -- the push and load PCs should be flagged
 *   - After the IPC read, workers do more private churn to ensure the IPC PCs
 *     survive amidst heavy private stack traffic
 *
 * Expected results (validated by run_tests.sh):
 *   - Strictly IPC > 0 (cross-thread sharing detected)
 *   - Unique Stack PCs with IPC >= 2 (at minimum the push PC and load PC)
 *
 * Stack lifetime stress:
 *   - The deep_churn() function creates nested stack frames with varying lifetimes
 *   - churn_with_reuse() repeatedly pushes/pops at the same logical stack depth,
 *     forcing the analyzer to handle address reuse across function call cycles
 */

#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <atomic>

// Shared communication layer
struct Comm {
    std::atomic<uint64_t*> ipc_ptr{nullptr};
    std::atomic<bool> flag{false};
    std::atomic<int> ready_threads{0};
};

Comm* g_comm = nullptr;
const int NUM_WORKERS = 4;
const int CHURN_ITERATIONS = 50000;

// Force compiler to avoid inlining to keep the PC distinct
__attribute__((noinline)) void dummy_churn() {
    volatile uint64_t a = 1;
    volatile uint64_t b = 2;
    volatile uint64_t c = a + b;
    (void)c;
}

__attribute__((noinline)) void deep_churn(int depth) {
    if (depth <= 0) return;
    volatile uint64_t local = depth;
    dummy_churn();
    deep_churn(depth - 1);
    local = local + 1;
}

// Repeatedly call functions that reuse the same stack depth,
// exercising address reuse / temporal correctness in the analyzer.
__attribute__((noinline)) void churn_with_reuse(int iterations) {
    for (int i = 0; i < iterations; i++) {
        // Each call to deep_churn(5) creates ~5 stack frames at the same
        // addresses as the previous call, forcing the analyzer to handle
        // push->pop->push->pop cycles at the same address.
        deep_churn(5);
    }
}

void* worker_thread(void* arg) {
    g_comm->ready_threads++;

    // Phase 1: Private churn while waiting for the anchor
    while (!g_comm->flag.load(std::memory_order_acquire)) {
        deep_churn(10);
    }

    // Phase 2: Read the IPC pointer (cross-thread stack access)
    uint64_t* ptr = g_comm->ipc_ptr.load(std::memory_order_acquire);
    if (ptr) {
        volatile uint64_t val = *ptr;
        (void)val;
    }

    // Phase 3: Heavy private churn with stack address reuse
    // This exercises the lifetime tracking and temporal correctness:
    // the same stack addresses cycle through push/pop many times.
    churn_with_reuse(CHURN_ITERATIONS);

    return nullptr;
}

__attribute__((noinline)) void establish_ipc() {
    // Allocate surrounding data to ensure the anchor is within a
    // well-defined region of private stack activity.
    volatile uint64_t padding_above[16];
    volatile uint64_t ipc_target = 42;  // THE ANCHOR
    volatile uint64_t padding_below[16];

    // Private work around the anchor
    for (int i = 0; i < 16; i++) {
        padding_above[i] = i;
        padding_below[i] = i;
    }

    // Expose the anchor to workers
    g_comm->ipc_ptr.store((uint64_t*)&ipc_target, std::memory_order_release);
    g_comm->flag.store(true, std::memory_order_release);

    // Keep the anchor alive while workers churn and read it.
    // Main thread also does private churn to add noise.
    churn_with_reuse(CHURN_ITERATIONS * 2);
}

int main() {
    g_comm = new Comm();

    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&workers[i], nullptr, worker_thread, nullptr);
    }

    // Wait for all workers to spin up and start churning
    while (g_comm->ready_threads.load() < NUM_WORKERS) {
        usleep(100);
    }

    // Establish IPC anchor on main thread stack and churn around it
    establish_ipc();

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], nullptr);
    }

    delete g_comm;
    return 0;
}
