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
const int CHURN_ITERATIONS = 100000;

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

void* worker_thread(void* arg) {
    g_comm->ready_threads++;
    
    // Phase 1: Private churn while waiting for flag
    while (!g_comm->flag.load(std::memory_order_acquire)) {
        deep_churn(10);
    }
    
    // Phase 2: Read the IPC pointer
    uint64_t* ptr = g_comm->ipc_ptr.load(std::memory_order_acquire);
    if (ptr) {
        // EXACTLY 1 IPC READ happens here
        volatile uint64_t val = *ptr;
        (void)val;
    }
    
    // Phase 3: More private churn
    for (int i = 0; i < CHURN_ITERATIONS; i++) {
        dummy_churn();
    }
    
    return nullptr;
}

__attribute__((noinline)) void establish_ipc() {
    // We allocate an array on the stack to surround our IPC variable with other data
    volatile uint64_t padding_above[16];
    volatile uint64_t ipc_target = 42; // THE ANCHOR
    volatile uint64_t padding_below[16];
    
    // Private churn above the anchor
    for (int i = 0; i < 16; i++) {
        padding_above[i] = i;
        padding_below[i] = i;
    }
    
    // Expose the anchor to workers
    g_comm->ipc_ptr.store((uint64_t*)&ipc_target, std::memory_order_release);
    g_comm->flag.store(true, std::memory_order_release);
    
    // Wait a bit while workers aggressively churn their stacks and read the anchor
    for (int i = 0; i < CHURN_ITERATIONS * 10; i++) {
        dummy_churn();
    }
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
