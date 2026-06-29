#include "analyzer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNACCESSED 0x00
#define SHARED_TAG 0xFF

static analyzer_callbacks_t g_cb;

// 3-Level Software Page Table for Shadow Memory
// L1: 2^16 entries (addr >> 32)
// L2: 2^16 entries ((addr >> 16) & 0xFFFF)
// L3: 2^16 bytes (addr & 0xFFFF)
static uint8_t ***shadow_dir = NULL;
static void *dir_lock = NULL;
static analyzer_stats_t g_stats;

static inline uint8_t* get_shadow_page(uintptr_t addr, bool allocate) {
    uintptr_t t1 = addr >> 32;
    uintptr_t t2 = (addr >> 16) & 0xFFFF;
    
    if (!shadow_dir[t1]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!shadow_dir[t1]) {
            shadow_dir[t1] = (uint8_t**)calloc(65536, sizeof(uint8_t*));
        }
        g_cb.mutex_unlock(dir_lock);
    }
    if (!shadow_dir[t1][t2]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!shadow_dir[t1][t2]) {
            shadow_dir[t1][t2] = (uint8_t*)calloc(65536, 1);
        }
        g_cb.mutex_unlock(dir_lock);
    }
    return (uint8_t*)shadow_dir[t1][t2];
}

// 1-Byte Offset Directory
static uint8_t ***offset_dir = NULL;

// 8-Byte Timestamp Directory
static uint64_t ***timestamp_dir = NULL;

static inline uint8_t* get_offset_page(uintptr_t addr, bool allocate) {
    uintptr_t t1 = addr >> 32;
    uintptr_t t2 = (addr >> 16) & 0xFFFF;
    
    if (!offset_dir[t1]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!offset_dir[t1]) {
            offset_dir[t1] = (uint8_t**)calloc(65536, sizeof(uint8_t*));
        }
        g_cb.mutex_unlock(dir_lock);
    }
    if (!offset_dir[t1][t2]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!offset_dir[t1][t2]) {
            offset_dir[t1][t2] = (uint8_t*)calloc(65536, 1);
        }
        g_cb.mutex_unlock(dir_lock);
    }
    return (uint8_t*)offset_dir[t1][t2];
}

static inline uint64_t* get_timestamp_page(uintptr_t addr, bool allocate) {
    uintptr_t t1 = addr >> 32;
    uintptr_t t2 = (addr >> 16) & 0xFFFF;
    
    if (!timestamp_dir[t1]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!timestamp_dir[t1]) {
            timestamp_dir[t1] = (uint64_t**)calloc(65536, sizeof(uint64_t*));
        }
        g_cb.mutex_unlock(dir_lock);
    }
    if (!timestamp_dir[t1][t2]) {
        if (!allocate) return NULL;
        g_cb.mutex_lock(dir_lock);
        if (!timestamp_dir[t1][t2]) {
            timestamp_dir[t1][t2] = (uint64_t*)calloc(65536, 8); // 8 bytes per slot
        }
        g_cb.mutex_unlock(dir_lock);
    }
    return (uint64_t*)timestamp_dir[t1][t2];
}

static analyzer_callbacks_t g_cb;
static analyzer_stats_t g_stats;

// Thread registry
typedef struct thread_info {
    bool active;
    uintptr_t stack_base;
    uintptr_t stack_top;
    uint64_t logical_clock;
} thread_info_t;

static thread_info_t g_threads[MAX_TID + 1];

#define NUM_HISTO_BINS 2049 // 0 to 2048, and one for >2048
static uint64_t global_lifetime_histogram[NUM_HISTO_BINS];

// L2 Hash Table
#define L2_HASH_BITS 20
#define L2_HASH_SIZE (1 << L2_HASH_BITS)
#define NUM_L2_LOCKS 4096

static void *l2_locks[NUM_L2_LOCKS];

typedef struct pc_access {
    uintptr_t pc;
    uint32_t count;
    bool is_push;
    struct pc_access *next;
} pc_access_t;

typedef struct addr_detail {
    uintptr_t addr;
    size_t size;
    uint8_t owner_tid;
    uint32_t push_count;
    uint32_t pop_count;
    pc_access_t *pc_list;
    struct addr_detail *next;
} addr_detail_t;

static addr_detail_t *l2_table[L2_HASH_SIZE];

static inline uint32_t hash_addr(uintptr_t addr) {
    uintptr_t x = addr >> 3;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x & (L2_HASH_SIZE - 1);
}

static inline int l2_lock_idx(uintptr_t addr) {
    return hash_addr(addr) & (NUM_L2_LOCKS - 1);
}

static addr_detail_t *l2_lookup(uintptr_t addr) {
    uint32_t h = hash_addr(addr);
    for (addr_detail_t *curr = l2_table[h]; curr; curr = curr->next) {
        if (curr->addr == addr) return curr;
    }
    return NULL;
}

static addr_detail_t *l2_create(uintptr_t addr, size_t size, uint8_t tid) {
    uint32_t h = hash_addr(addr);
    addr_detail_t *n = (addr_detail_t *)calloc(1, sizeof(addr_detail_t));
    n->addr = addr;
    n->size = size;
    n->owner_tid = tid;
    n->next = l2_table[h];
    l2_table[h] = n;
    g_stats.l2_entries_created++;
    return n;
}

static void l2_delete(uintptr_t addr) {
    uint32_t h = hash_addr(addr);
    addr_detail_t *prev = NULL;
    for (addr_detail_t *curr = l2_table[h]; curr; prev = curr, curr = curr->next) {
        if (curr->addr == addr) {
            if (prev) prev->next = curr->next;
            else      l2_table[h] = curr->next;
            
            pc_access_t *p = curr->pc_list;
            while (p) {
                pc_access_t *next = p->next;
                free(p);
                p = next;
            }
            free(curr);
            return;
        }
    }
}

static void l2_delete_if_owner(uintptr_t addr, uint8_t tid) {
    int lock_idx = l2_lock_idx(addr);
    g_cb.mutex_lock(l2_locks[lock_idx]);
    addr_detail_t *detail = l2_lookup(addr);
    if (detail && detail->owner_tid == tid) {
        l2_delete(addr);
    }
    g_cb.mutex_unlock(l2_locks[lock_idx]);
}

static void l2_update_exact(uintptr_t addr, size_t size, uint8_t tid, uintptr_t pc, bool is_push) {
    int lock_idx = l2_lock_idx(addr);
    g_cb.mutex_lock(l2_locks[lock_idx]);
    
    addr_detail_t *detail = l2_lookup(addr);
    if (!detail) {
        detail = l2_create(addr, size, tid);
    } else {
        detail->size = size;
    }
    
    if (is_push) detail->push_count++;
    else         detail->pop_count++;
    
    for (pc_access_t *p = detail->pc_list; p; p = p->next) {
        if (p->pc == pc && p->is_push == is_push) {
            p->count++;
            
            // Map offsets
            for (size_t i = 0; i < size; i++) {
                uint8_t *offset_page = get_offset_page(addr + i, true);
                offset_page[(addr + i) & 0xFFFF] = (uint8_t)i;
            }
            g_cb.mutex_unlock(l2_locks[lock_idx]);
            return;
        }
    }
    
    pc_access_t *p = (pc_access_t *)malloc(sizeof(pc_access_t));
    p->pc = pc;
    p->is_push = is_push;
    p->count = 1;
    p->next = detail->pc_list;
    detail->pc_list = p;
    
    // Map offsets
    for (size_t i = 0; i < size; i++) {
        uint8_t *offset_page = get_offset_page(addr + i, true);
        offset_page[(addr + i) & 0xFFFF] = (uint8_t)i;
    }
    g_cb.mutex_unlock(l2_locks[lock_idx]);
}

static void drain_l2(uintptr_t curr_addr) {
    uint8_t *offset_page = get_offset_page(curr_addr, false);
    if (!offset_page) return;
    
    uint8_t offset = offset_page[curr_addr & 0xFFFF];
    uintptr_t base_addr = curr_addr - offset;
    
    int lock_idx = l2_lock_idx(base_addr);
    g_cb.mutex_lock(l2_locks[lock_idx]);
    
    addr_detail_t *detail = l2_lookup(base_addr);
    if (!detail || curr_addr >= detail->addr + detail->size) {
        g_cb.mutex_unlock(l2_locks[lock_idx]);
        return; 
    }
    
    if (g_cb.add_strict_ipc) {
        g_cb.add_strict_ipc(detail->owner_tid, detail->push_count, detail->pop_count);
    }
    
    if (g_cb.mark_pc_ipc) {
        for (pc_access_t *p = detail->pc_list; p; p = p->next) {
            g_cb.mark_pc_ipc(p->pc, p->is_push);
        }
    }
    
    g_stats.l2_entries_drained++;
    l2_delete(base_addr);
    g_cb.mutex_unlock(l2_locks[lock_idx]);
}

static inline bool is_my_stack(uint8_t tid, uintptr_t addr) {
    if (!g_threads[tid].active) return false;
    return addr >= g_threads[tid].stack_base && addr < g_threads[tid].stack_top;
}

static inline uint64_t get_tid_broadcast(uint8_t tid) {
    uint64_t t = tid;
    return t | (t << 8) | (t << 16) | (t << 24) | 
           (t << 32) | (t << 40) | (t << 48) | (t << 56);
}

static void handle_mixed_bytes(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc, bool is_push, bool is_stack_op) {
    bool newly_shared = false;
    bool already_shared = false;
    
    for (size_t i = 0; i < size; i++) {
        uintptr_t curr_addr = addr + i;
        uint8_t *page = get_shadow_page(curr_addr, true);
        uint8_t *s = &page[curr_addr & 0xFFFF];
        
        uint8_t old = *s;
        while (true) {
            if (old == SHARED_TAG) {
                already_shared = true;
                break;
            }
            if (old == tid) {
                break;
            }
            
            uint8_t new_val = (old == UNACCESSED) ? tid : SHARED_TAG;
            uint8_t prev = __sync_val_compare_and_swap(s, old, new_val);
            if (prev == old) {
                if (new_val == SHARED_TAG) {
                    newly_shared = true;
                    drain_l2(curr_addr);
                }
                break;
            }
            old = prev;
        }
    }
    
    if (is_stack_op) {
        if (newly_shared || already_shared) {
            if (g_cb.mark_pc_ipc) {
                g_cb.mark_pc_ipc(pc, is_push);
            }
            if (g_cb.add_strict_ipc) {
                g_cb.add_strict_ipc(tid, is_push ? 1 : 0, is_push ? 0 : 1);
            }
        } else {
            l2_update_exact(addr, size, tid, pc, is_push);
        }
    }
}

void analyzer_init(analyzer_callbacks_t cb) {
    g_cb = cb;
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_threads, 0, sizeof(g_threads));
    memset(l2_table, 0, sizeof(l2_table));
    
    dir_lock = g_cb.mutex_create();
    for (int i = 0; i < NUM_L2_LOCKS; i++) {
        l2_locks[i] = g_cb.mutex_create();
    }
    
    shadow_dir = (uint8_t***)calloc(65536, sizeof(uint8_t**));
    offset_dir = (uint8_t***)calloc(65536, sizeof(uint8_t**));
    timestamp_dir = (uint64_t***)calloc(65536, sizeof(uint64_t**));
}

void analyzer_register_thread(uint8_t tid, uintptr_t stack_base, uintptr_t stack_top) {
    if (tid == 0 || tid > MAX_TID) return;
    g_threads[tid].active = true;
    g_threads[tid].stack_base = stack_base;
    g_threads[tid].stack_top = stack_top;
}

void analyzer_update_clock(uint8_t tid, uint32_t ticks) {
    if (tid == 0 || tid > MAX_TID || !g_threads[tid].active) return;
    g_threads[tid].logical_clock += ticks;
}

void analyzer_unregister_thread(uint8_t tid) {
    if (tid == 0 || tid > MAX_TID) return;
    g_threads[tid].active = false;
}

#include <assert.h>

void analyzer_on_push(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc) {
    assert(size <= 256 && "Memory access size exceeds 1-byte offset capacity");
    g_stats.total_push++;
    
    uint8_t *page = get_shadow_page(addr, true);
    uint32_t offset = addr & 0xFFFF;
    
    // Record timestamp
    uint64_t clock = g_threads[tid].logical_clock;
    uint64_t *ts_page = get_timestamp_page(addr, true);
    ts_page[offset] = clock;
    
    if (page[offset] == SHARED_TAG && is_my_stack(tid, addr)) {
        // Reset lifetime
        for (size_t i = 0; i < size; i++) {
            uintptr_t curr_addr = addr + i;
            uint8_t *p = get_shadow_page(curr_addr, true);
            p[curr_addr & 0xFFFF] = tid;
            
            // Delete any exact L2 entries in this range
            l2_delete_if_owner(curr_addr, tid);
        }
    }
    
    l2_delete_if_owner(addr, tid);
    
    if (size == 8 && (offset + 8 <= 65536)) {
        uint64_t my_tid_8 = get_tid_broadcast(tid);
        uint64_t *shadow_8 = (uint64_t*)&page[offset];
        uint64_t old_val = *shadow_8;
        if (old_val == 0ULL) {
            if (__sync_bool_compare_and_swap(shadow_8, 0ULL, my_tid_8)) {
                l2_update_exact(addr, size, tid, pc, true);
                return;
            }
        } else if (old_val == my_tid_8) {
            l2_update_exact(addr, size, tid, pc, true);
            return;
        }
    }
    
    handle_mixed_bytes(tid, addr, size, pc, true, true);
}

void analyzer_on_pop(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc) {
    assert(size <= 256 && "Memory access size exceeds 1-byte offset capacity");
    g_stats.total_pop++;
    uint8_t *page = get_shadow_page(addr, true);
    uint32_t offset = addr & 0xFFFF;
    
    // Extract base address from offset dir
    uint8_t *offset_page = get_offset_page(addr, false);
    if (offset_page) {
        uint8_t push_offset = offset_page[offset];
        uintptr_t base_addr = addr - push_offset;
        uint64_t *ts_page = get_timestamp_page(base_addr, false);
        if (ts_page) {
            uint64_t push_time = ts_page[base_addr & 0xFFFF];
            uint64_t current_time = g_threads[tid].logical_clock;
            if (current_time >= push_time) {
                uint64_t lifetime = current_time - push_time;
                if (lifetime > 2048) lifetime = 2048; // cap to highest bin
                __sync_fetch_and_add(&global_lifetime_histogram[lifetime], 1);
            }
        }
    }
    
    if (size == 8 && (offset + 8 <= 65536)) {
        uint64_t my_tid_8 = get_tid_broadcast(tid);
        uint64_t *shadow_8 = (uint64_t*)&page[offset];
        uint64_t old_val = *shadow_8;
        if (old_val == 0ULL) {
            if (__sync_bool_compare_and_swap(shadow_8, 0ULL, my_tid_8)) {
                l2_update_exact(addr, size, tid, pc, false);
                return;
            }
        } else if (old_val == my_tid_8) {
            l2_update_exact(addr, size, tid, pc, false);
            return;
        }
    }
    
    handle_mixed_bytes(tid, addr, size, pc, false, true);
}

void analyzer_on_ld(uint8_t tid, uintptr_t addr, size_t size) {
    g_stats.total_ld++;
    uint8_t *page = get_shadow_page(addr, true);
    uint32_t offset = addr & 0xFFFF;
    
    if (size == 8 && (offset + 8 <= 65536)) {
        uint64_t my_tid_8 = get_tid_broadcast(tid);
        uint64_t *shadow_8 = (uint64_t*)&page[offset];
        uint64_t old_val = *shadow_8;
        if (old_val == 0ULL) {
            if (__sync_bool_compare_and_swap(shadow_8, 0ULL, my_tid_8)) {
                return;
            }
        } else if (old_val == my_tid_8) {
            return;
        }
    }
    
    handle_mixed_bytes(tid, addr, size, 0, false, false);
}

void analyzer_on_st(uint8_t tid, uintptr_t addr, size_t size) {
    g_stats.total_st++;
    uint8_t *page = get_shadow_page(addr, true);
    uint32_t offset = addr & 0xFFFF;
    
    if (size == 8 && (offset + 8 <= 65536)) {
        uint64_t my_tid_8 = get_tid_broadcast(tid);
        uint64_t *shadow_8 = (uint64_t*)&page[offset];
        uint64_t old_val = *shadow_8;
        if (old_val == 0ULL) {
            if (__sync_bool_compare_and_swap(shadow_8, 0ULL, my_tid_8)) {
                return;
            }
        } else if (old_val == my_tid_8) {
            return;
        }
    }
    
    handle_mixed_bytes(tid, addr, size, 0, false, false);
}

void analyzer_get_stats(analyzer_stats_t *out) {
    if (out) *out = g_stats;
}

void analyzer_cleanup(void) {
    if (shadow_dir) {
        for (int i = 0; i < 65536; i++) {
            if (shadow_dir[i]) {
                for (int j = 0; j < 65536; j++) {
                    if (shadow_dir[i][j]) {
                        free(shadow_dir[i][j]);
                    }
                }
                free(shadow_dir[i]);
            }
        }
        free(shadow_dir);
        shadow_dir = NULL;
    }
    
    if (offset_dir) {
        for (int i = 0; i < 65536; i++) {
            if (offset_dir[i]) {
                for (int j = 0; j < 65536; j++) {
                    if (offset_dir[i][j]) {
                        free(offset_dir[i][j]);
                    }
                }
                free(offset_dir[i]);
            }
        }
        free(offset_dir);
        offset_dir = NULL;
    }
    
    if (timestamp_dir) {
        for (int i = 0; i < 65536; i++) {
            if (timestamp_dir[i]) {
                for (int j = 0; j < 65536; j++) {
                    if (timestamp_dir[i][j]) {
                        free(timestamp_dir[i][j]);
                    }
                }
                free(timestamp_dir[i]);
            }
        }
        free(timestamp_dir);
        timestamp_dir = NULL;
    }
    
    for (int i = 0; i < L2_HASH_SIZE; i++) {
        addr_detail_t *curr = l2_table[i];
        while (curr) {
            addr_detail_t *next = curr->next;
            pc_access_t *p = curr->pc_list;
            while (p) {
                pc_access_t *pn = p->next;
                free(p);
                p = pn;
            }
            free(curr);
            curr = next;
        }
        l2_table[i] = NULL;
    }
    
    if (g_cb.mutex_destroy && dir_lock) {
        g_cb.mutex_destroy(dir_lock);
        dir_lock = NULL;
    }
    for (int i = 0; i < NUM_L2_LOCKS; i++) {
        if (g_cb.mutex_destroy && l2_locks[i]) {
            g_cb.mutex_destroy(l2_locks[i]);
            l2_locks[i] = NULL;
        }
    }
}

const uint64_t* analyzer_get_histogram(void) {
    return global_lifetime_histogram;
}
