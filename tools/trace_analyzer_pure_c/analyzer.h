#ifndef ANALYZER_H
#define ANALYZER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TID 254

// Callbacks provided by the host environment (DynamoRIO or Test Harness)
typedef struct {
    // Called when a PC is retroactively or directly determined to be involved in IPC.
    void (*mark_pc_ipc)(uintptr_t pc, bool is_push);
    
    // Called when retroactive sharing is detected to add counts to a thread's strict IPC metric.
    void (*add_strict_ipc)(uint8_t tid, uint32_t pushes, uint32_t pops);
    
    // Debug logging callback.
    void (*log_debug)(const char *msg);
    
    // Mutex abstraction for thread-safety across environments (DynamoRIO vs pthreads)
    void* (*mutex_create)(void);
    void (*mutex_lock)(void *mutex);
    void (*mutex_unlock)(void *mutex);
    void (*mutex_destroy)(void *mutex);
} analyzer_callbacks_t;

// Statistics collected by the analyzer internally.
typedef struct {
    uint64_t total_ld;
    uint64_t total_st;
    uint64_t total_push;
    uint64_t total_pop;
    uint64_t l2_entries_created;
    uint64_t l2_entries_drained;
} analyzer_stats_t;

// Initialize the analyzer subsystem.
void analyzer_init(analyzer_callbacks_t cb);

// Register a thread and its stack boundaries.
void analyzer_register_thread(uint8_t tid, uintptr_t stack_base, uintptr_t stack_top);

// Unregister a thread.
void analyzer_unregister_thread(uint8_t tid);

// Update a thread's logical clock
void analyzer_update_clock(uint8_t tid, uint32_t ticks);

// The core event handlers.
void analyzer_on_push(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc);
void analyzer_on_pop (uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc);
void analyzer_on_ld  (uint8_t tid, uintptr_t addr, size_t size);
void analyzer_on_st  (uint8_t tid, uintptr_t addr, size_t size);

// Retrieve internal statistics.
void analyzer_get_stats(analyzer_stats_t *out);

// Cleanup all resources (frees shadow memory and hash tables).
void analyzer_cleanup(void);

// Get the histogram array of size 2049
const uint64_t* analyzer_get_histogram(void);

#ifdef __cplusplus
}
#endif

#endif // ANALYZER_H
