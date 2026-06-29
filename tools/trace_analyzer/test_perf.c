#include "analyzer.h"
#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 4
#define NUM_ITERS 10000000

void cb_mark_pc_ipc(uintptr_t pc, bool is_push) {}
void cb_add_strict_ipc(uint8_t tid, uint32_t pushes, uint32_t pops) {}
void cb_log_debug(const char *msg) {}

void* worker(void* arg) {
    uintptr_t tid = (uintptr_t)arg;
    uintptr_t base = 0x10000000 + (tid * 0x1000000);
    
    for (int i = 0; i < NUM_ITERS; i++) {
        uintptr_t addr = base + ((i % 1024) * 8);
        analyzer_on_push(tid, addr, 8, 0x4000);
        analyzer_on_ld(tid, addr, 8);
        analyzer_on_pop(tid, addr, 8, 0x4008);
    }
    return NULL;
}

int main() {
    analyzer_callbacks_t cb = { cb_mark_pc_ipc, cb_add_strict_ipc, cb_log_debug };
    analyzer_init(cb);
    for (int i = 0; i < NUM_THREADS; i++) {
        analyzer_register_thread(i, 0x10000000 + (i * 0x1000000), 0x10000000 + ((i+1) * 0x1000000));
    }
    
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker, (void*)(uintptr_t)i);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    return 0;
}
