#define unlikely(x) __builtin_expect(!!(x), 0)
#include "analyzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <cassert>

#define UNACCESSED 0x00
#define SHARED_TAG 0x80 // MSB set

static analyzer_callbacks_t g_cb;

// ---------------------------------------------------------
// Global Stats & PC Tracking
// ---------------------------------------------------------
#define CTX_HASH_BITS 3
#define CTX_BUCKETS (1 << CTX_HASH_BITS)

struct pc_history_entry_t {
    std::atomic<uint64_t> total_count{0};
    std::atomic<bool> is_ipc{false};
};

struct alignas(64) pc_global_t {
    pc_history_entry_t history[CTX_BUCKETS];
    bool is_push{false};
};

#define NUM_PC_BANKS 64
struct pc_bank_t {
    void* lock;
    std::unordered_map<uintptr_t, pc_global_t> map;
};
static pc_bank_t pc_banks[NUM_PC_BANKS];

static std::atomic<uint64_t> stat_total_ld{0};
static std::atomic<uint64_t> stat_total_st{0};
static std::atomic<uint64_t> stat_total_push{0};
static std::atomic<uint64_t> stat_total_pop{0};
static std::atomic<uint64_t> stat_l2_created{0};
static std::atomic<uint64_t> stat_l2_drained{0};

// ---------------------------------------------------------
// Thread Registry
// ---------------------------------------------------------
struct thread_info_t {
    bool active;
    uintptr_t stack_base;
    uintptr_t stack_top;
    uint64_t logical_clock;
};
static thread_info_t g_threads[MAX_TID + 1];

#define NUM_HISTO_BINS 2049
static std::atomic<uint64_t> global_lifetime_histogram[NUM_HISTO_BINS];

// ---------------------------------------------------------
// Shadow Directory (2-Level, 2MB Top -> 2GB Pages)
// ---------------------------------------------------------
struct shadow_info_t {
    uint8_t owner_and_flags;
    uint8_t offset;
};

// Top level covers 48-bit address space. 1GB chunks -> 2^18 entries
#define SHADOW_TOP_BITS 18
#define SHADOW_TOP_ENTRIES (1 << SHADOW_TOP_BITS)
#define SHADOW_OFFSET_BITS 30
#define SHADOW_OFFSET_MASK ((1ULL << SHADOW_OFFSET_BITS) - 1)

static shadow_info_t** shadow_dir = nullptr;
static void* dir_mutex = nullptr;

static inline shadow_info_t* get_shadow_info(uintptr_t addr, bool allocate) {
    uint32_t top = (addr >> SHADOW_OFFSET_BITS) & (SHADOW_TOP_ENTRIES - 1);
    
    if (!shadow_dir[top]) {
        if (!allocate) return nullptr;
        if (g_cb.mutex_lock && dir_mutex) g_cb.mutex_lock(dir_mutex);
        if (!shadow_dir[top]) {
            size_t page_size = (1ULL << SHADOW_OFFSET_BITS) * sizeof(shadow_info_t); // 2GB
            void* page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, 
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (page == MAP_FAILED) {
                fprintf(stderr, "TraceAnalyzer: mmap failed for shadow memory!\n");
                if (g_cb.mutex_unlock && dir_mutex) g_cb.mutex_unlock(dir_mutex);
                return nullptr;
            }
            shadow_dir[top] = (shadow_info_t*)page;
        }
        if (g_cb.mutex_unlock && dir_mutex) g_cb.mutex_unlock(dir_mutex);
    }
    return &shadow_dir[top][addr & SHADOW_OFFSET_MASK];
}

// ---------------------------------------------------------
// L2 Hash Table
// ---------------------------------------------------------
#define L2_HASH_BITS 20
#define L2_HASH_SIZE (1 << L2_HASH_BITS)
#define NUM_L2_LOCKS 4096

static void* l2_locks[NUM_L2_LOCKS];

struct pc_access_t {
    uintptr_t pc;
    uint32_t count;
    bool is_push;
    uint8_t ctx_hash;
};

struct addr_detail_t {
    uintptr_t addr;
    size_t size;
    uint8_t owner_tid;
    uint32_t push_count;
    uint32_t pop_count;
    uint64_t created_clock;
    std::vector<pc_access_t> pc_list;
    addr_detail_t *next;
};

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

static addr_detail_t* l2_lookup(uintptr_t addr) {
    uint32_t h = hash_addr(addr);
    addr_detail_t *curr = l2_table[h];
    while (curr) {
        if (curr->addr == addr) return curr;
        curr = curr->next;
    }
    return nullptr;
}

static addr_detail_t* l2_create(uintptr_t addr, size_t size, uint8_t tid) {
    uint32_t h = hash_addr(addr);
    addr_detail_t *n = new addr_detail_t();
    n->addr = addr;
    n->size = size;
    n->owner_tid = tid;
    n->push_count = 0;
    n->pop_count = 0;
    n->created_clock = g_threads[tid].logical_clock;
    n->next = l2_table[h];
    l2_table[h] = n;
    stat_l2_created.fetch_add(1, std::memory_order_relaxed);
    return n;
}

static void l2_delete(uintptr_t addr) {
    uint32_t h = hash_addr(addr);
    addr_detail_t *curr = l2_table[h];
    addr_detail_t *prev = nullptr;
    while (curr) {
        if (curr->addr == addr) {
            if (prev) prev->next = curr->next;
            else      l2_table[h] = curr->next;
            
            // Record lifetime
                        if (curr->created_clock != UINT64_MAX) {
                uint64_t current_time = g_threads[curr->owner_tid].logical_clock;
                uint64_t lifetime = (current_time > curr->created_clock) ? (current_time - curr->created_clock) : 0;
                if (lifetime > 2048) lifetime = 2048;
                global_lifetime_histogram[lifetime].fetch_add(1, std::memory_order_relaxed);
            }
            
            delete curr;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// ---------------------------------------------------------
// Analyzer API
// ---------------------------------------------------------
void analyzer_init(analyzer_callbacks_t *cb) {
    g_cb = *cb;
    if (g_cb.mutex_create) {
        for (int i = 0; i < NUM_L2_LOCKS; i++) {
            l2_locks[i] = g_cb.mutex_create();
        }
        for (int i = 0; i < NUM_PC_BANKS; i++) {
            pc_banks[i].lock = g_cb.mutex_create();
        }
        dir_mutex = g_cb.mutex_create();
    }

    shadow_dir = (shadow_info_t**)calloc(SHADOW_TOP_ENTRIES, sizeof(shadow_info_t*));
    for (int i = 0; i < NUM_PC_BANKS; i++) {
        pc_banks[i].map.reserve(100000 / NUM_PC_BANKS);
    }
}

void analyzer_register_thread(uint8_t tid, uintptr_t stack_base, uintptr_t stack_top) {
    if (tid > MAX_TID) return;
    g_threads[tid].active = true;
    g_threads[tid].stack_base = stack_base;
    g_threads[tid].stack_top = stack_top;
    g_threads[tid].logical_clock = 0;
}

void analyzer_unregister_thread(uint8_t tid) {
    if (tid > MAX_TID) return;
    g_threads[tid].active = false;
}

void analyzer_add_logical_clock(uint8_t tid, uint32_t ticks) {
    if (tid > MAX_TID) return;
    g_threads[tid].logical_clock += ticks;
}

static inline void record_pc(uintptr_t pc, bool is_push, uint8_t ctx_hash) {
    uint32_t bank = (pc >> 2) % NUM_PC_BANKS;
    if (g_cb.mutex_lock) g_cb.mutex_lock(pc_banks[bank].lock);
    auto it = pc_banks[bank].map.find(pc);
    if (it == pc_banks[bank].map.end()) {
        auto& g = pc_banks[bank].map[pc];
        g.is_push = is_push;
        g.history[ctx_hash].total_count.store(1, std::memory_order_relaxed);
    } else {
        it->second.history[ctx_hash].total_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(pc_banks[bank].lock);
}

static void l2_update_exact(uintptr_t addr, size_t size, uint8_t tid, uintptr_t pc, bool is_push, uint8_t ctx_hash) {
    int lock_idx = l2_lock_idx(addr);
    if (g_cb.mutex_lock) g_cb.mutex_lock(l2_locks[lock_idx]);
    
    addr_detail_t *detail = l2_lookup(addr);
    bool was_created = false;
    if (!detail) {
        detail = l2_create(addr, size, tid);
        was_created = true;
    } else {
        detail->size = size;
    }
    
    if (is_push) {
        detail->push_count++;
    } else {
        detail->pop_count++;
        if (was_created) detail->created_clock = UINT64_MAX;
        if (detail->created_clock != UINT64_MAX) {
            uint64_t current_time = g_threads[tid].logical_clock;
            uint64_t lifetime = (current_time > detail->created_clock) ? (current_time - detail->created_clock) : 0;
            if (lifetime > 2048) lifetime = 2048;
            global_lifetime_histogram[lifetime].fetch_add(1, std::memory_order_relaxed);
            detail->created_clock = UINT64_MAX;
        }
    }
    
    for (auto& p : detail->pc_list) {
        if (p.pc == pc && p.is_push == is_push && p.ctx_hash == ctx_hash) {
            p.count++;
            shadow_info_t* base_info = get_shadow_info(addr, true);
            if (base_info) {
                if (unlikely((addr & SHADOW_OFFSET_MASK) + size > (1ULL << SHADOW_OFFSET_BITS))) {
                    for (size_t i = 0; i < size; i++) {
                        shadow_info_t* info = get_shadow_info(addr + i, true);
                        if (info) info->offset = (uint8_t)i;
                    }
                } else {
                    for (size_t i = 0; i < size; i++) {
                        base_info[i].offset = (uint8_t)i;
                    }
                }
            }
            if (g_cb.mutex_unlock) g_cb.mutex_unlock(l2_locks[lock_idx]); return;
        }
    }
    
    detail->pc_list.push_back({pc, 1, is_push, ctx_hash});
    
    shadow_info_t* base_info_2 = get_shadow_info(addr, true);
    if (base_info_2) {
        if (unlikely((addr & SHADOW_OFFSET_MASK) + size > (1ULL << SHADOW_OFFSET_BITS))) {
            for (size_t i = 0; i < size; i++) {
                shadow_info_t* info = get_shadow_info(addr + i, true);
                if (info) info->offset = (uint8_t)i;
            }
        } else {
            for (size_t i = 0; i < size; i++) {
                base_info_2[i].offset = (uint8_t)i;
            }
        }
    }
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(l2_locks[lock_idx]);
}

static void silent_delete_l2(uintptr_t base_addr) {
    int lock_idx = l2_lock_idx(base_addr);
    if (g_cb.mutex_lock) g_cb.mutex_lock(l2_locks[lock_idx]);
    l2_delete(base_addr);
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(l2_locks[lock_idx]);
}

static inline void mark_pc_ipc(uintptr_t pc, uint8_t ctx_hash) {
    uint32_t bank = (pc >> 2) % NUM_PC_BANKS;
    if (g_cb.mutex_lock) g_cb.mutex_lock(pc_banks[bank].lock);
    auto it = pc_banks[bank].map.find(pc);
    if (it != pc_banks[bank].map.end()) {
        it->second.history[ctx_hash].is_ipc.store(true, std::memory_order_relaxed);
    }
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(pc_banks[bank].lock);
}


static void retroactive_ipc_and_drain_l2(uintptr_t curr_addr) {
    shadow_info_t* info = get_shadow_info(curr_addr, false);
    if (!info) return;
    
    uint8_t offset = info->offset;
    uintptr_t base_addr = curr_addr - offset;
    
    int lock_idx = l2_lock_idx(base_addr);
    if (g_cb.mutex_lock) g_cb.mutex_lock(l2_locks[lock_idx]);
    
    addr_detail_t *detail = l2_lookup(base_addr);
    if (!detail) {
        if (g_cb.mutex_unlock) g_cb.mutex_unlock(l2_locks[lock_idx]);
        return;
    }
    
    for (auto& p : detail->pc_list) {
        mark_pc_ipc(p.pc, p.ctx_hash);
    }
    
    if (g_cb.add_strict_ipc) {
        g_cb.add_strict_ipc(detail->owner_tid, detail->push_count, detail->pop_count);
    }
    
    stat_l2_drained.fetch_add(1, std::memory_order_relaxed);
    l2_delete(base_addr);
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(l2_locks[lock_idx]);
}

void analyzer_on_push(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc, uint8_t ctx_hash) {
    assert(size <= 256 && "Memory access size exceeds 1-byte offset capacity");
    stat_total_push.fetch_add(1, std::memory_order_relaxed);
    record_pc(pc, true, ctx_hash);
    
    shadow_info_t* base_info = get_shadow_info(addr, true);
    if (!base_info) return;
    
    if (base_info->owner_and_flags != UNACCESSED) {
        silent_delete_l2(addr);
    }
    
    l2_update_exact(addr, size, tid, pc, true, ctx_hash);
    
    if (unlikely((addr & SHADOW_OFFSET_MASK) + size > (1ULL << SHADOW_OFFSET_BITS))) {
        for (size_t i = 0; i < size; i++) {
            shadow_info_t* info = get_shadow_info(addr + i, true);
            if (info) {
                info->owner_and_flags = tid;
                info->offset = (uint8_t)i;
            }
        }
        return;
    }
    
    for (size_t i = 0; i < size; i++) {
        base_info[i].owner_and_flags = tid;
        base_info[i].offset = (uint8_t)i;
    }
}

void analyzer_on_pop(uint8_t tid, uintptr_t addr, size_t size, uintptr_t pc, uint8_t ctx_hash) {
    assert(size <= 256 && "Memory access size exceeds 1-byte offset capacity");
    stat_total_pop.fetch_add(1, std::memory_order_relaxed);
    record_pc(pc, false, ctx_hash);
    
    l2_update_exact(addr, size, tid, pc, false, ctx_hash);
}

static inline void handle_mixed_bytes(uint8_t tid, uintptr_t addr, size_t size) {
    shadow_info_t* base_info = get_shadow_info(addr, false);
    if (!base_info) return;
    
    if (unlikely((addr & SHADOW_OFFSET_MASK) + size > (1ULL << SHADOW_OFFSET_BITS))) {
        for (size_t i = 0; i < size; i++) {
            shadow_info_t* info = get_shadow_info(addr + i, false);
            if (!info) continue;
            uint8_t owner = info->owner_and_flags & 0x7F;
            if (owner != UNACCESSED && owner != tid && !(info->owner_and_flags & SHARED_TAG)) {
                if (__sync_bool_compare_and_swap(&info->owner_and_flags, info->owner_and_flags, info->owner_and_flags | SHARED_TAG)) {
                    retroactive_ipc_and_drain_l2(addr + i);
                }
            }
        }
        return;
    }
    
    for (size_t i = 0; i < size; i++) {
        uint8_t old_val = base_info[i].owner_and_flags;
        uint8_t owner = old_val & 0x7F;
        if (owner != UNACCESSED && owner != tid && !(old_val & SHARED_TAG)) {
            if (__sync_bool_compare_and_swap(&base_info[i].owner_and_flags, old_val, old_val | SHARED_TAG)) {
                retroactive_ipc_and_drain_l2(addr + i);
            }
        }
    }
}

void analyzer_on_ld(uint8_t tid, uintptr_t addr, size_t size) {
    stat_total_ld.fetch_add(1, std::memory_order_relaxed);
    handle_mixed_bytes(tid, addr, size);
}

void analyzer_on_st(uint8_t tid, uintptr_t addr, size_t size) {
    stat_total_st.fetch_add(1, std::memory_order_relaxed);
    handle_mixed_bytes(tid, addr, size);
}

void analyzer_get_stats(analyzer_stats_t *out) {
    if (out) {
        out->total_ld = stat_total_ld.load();
        out->total_st = stat_total_st.load();
        out->total_push = stat_total_push.load();
        out->total_pop = stat_total_pop.load();
        out->l2_entries_created = stat_l2_created.load();
        out->l2_entries_drained = stat_l2_drained.load();
    }
}

const uint64_t* analyzer_get_histogram(void) {
    static uint64_t flat_hist[NUM_HISTO_BINS];
    for (int i = 0; i < NUM_HISTO_BINS; i++) {
        flat_hist[i] = global_lifetime_histogram[i].load();
    }
    return flat_hist;
}

bool analyzer_test_is_pc_ipc(uintptr_t pc) {
    uint32_t bank = (pc >> 2) % NUM_PC_BANKS;
    bool res = false;
    if (g_cb.mutex_lock) g_cb.mutex_lock(pc_banks[bank].lock);
    auto it = pc_banks[bank].map.find(pc);
    if (it != pc_banks[bank].map.end()) {
        for (int i=0; i<CTX_BUCKETS; i++) {
            if (it->second.history[i].is_ipc.load(std::memory_order_relaxed)) {
                res = true;
                break;
            }
        }
    }
    if (g_cb.mutex_unlock) g_cb.mutex_unlock(pc_banks[bank].lock);
    return res;
}

// Extract PC tracking stats from banks
void analyzer_get_pc_stats(uint64_t* sum_possible_push, uint64_t* sum_possible_pop, uint64_t* hist_sum_push, uint64_t* hist_sum_pop, uint32_t* unique_pcs, uint32_t* unique_ipc_pcs, bool try_lock_only) {
    *sum_possible_push = 0;
    *sum_possible_pop = 0;
    *hist_sum_push = 0;
    *hist_sum_pop = 0;
    *unique_pcs = 0;
    *unique_ipc_pcs = 0;
    
    for (int i = 0; i < NUM_PC_BANKS; i++) {
        if (try_lock_only && g_cb.mutex_trylock) {
            if (!g_cb.mutex_trylock(pc_banks[i].lock)) continue;
        } else {
            if (g_cb.mutex_lock) g_cb.mutex_lock(pc_banks[i].lock);
        }
        for (const auto& pair : pc_banks[i].map) {
            (*unique_pcs)++;
            
            bool any_ipc = false;
            uint64_t raw_total_count = 0;
            uint64_t hist_tainted_count = 0;
            
            for (int b = 0; b < CTX_BUCKETS; b++) {
                uint64_t c = pair.second.history[b].total_count.load(std::memory_order_relaxed);
                raw_total_count += c;
                if (pair.second.history[b].is_ipc.load(std::memory_order_relaxed)) {
                    any_ipc = true;
                    hist_tainted_count += c;
                }
            }
            
            if (any_ipc) {
                (*unique_ipc_pcs)++;
                if (pair.second.is_push) {
                    *sum_possible_push += raw_total_count;
                    *hist_sum_push += hist_tainted_count;
                } else {
                    *sum_possible_pop += raw_total_count;
                    *hist_sum_pop += hist_tainted_count;
                }
            }
        }
        if (g_cb.mutex_unlock) g_cb.mutex_unlock(pc_banks[i].lock);
    }
}

void analyzer_drain_hanging_pushes(void) {
    for (int i = 0; i < L2_HASH_SIZE; i++) {
        addr_detail_t *curr = l2_table[i];
        while (curr) {
            if (curr->created_clock != UINT64_MAX) {
                            if (curr->created_clock != UINT64_MAX) {
                uint64_t current_time = g_threads[curr->owner_tid].logical_clock;
                uint64_t lifetime = (current_time > curr->created_clock) ? (current_time - curr->created_clock) : 0;
                if (lifetime > 2048) lifetime = 2048;
                global_lifetime_histogram[lifetime].fetch_add(1, std::memory_order_relaxed);
            }
                curr->created_clock = UINT64_MAX; // Prevent double-counting
            }
            curr = curr->next;
        }
    }
}

void analyzer_cleanup(void) {
    if (shadow_dir) {
        for (uint32_t i = 0; i < SHADOW_TOP_ENTRIES; i++) {
            if (shadow_dir[i]) {
                size_t page_size = (1ULL << SHADOW_OFFSET_BITS) * sizeof(shadow_info_t);
                munmap(shadow_dir[i], page_size);
            }
        }
        free(shadow_dir);
        shadow_dir = nullptr;
    }
    
    for (int i = 0; i < L2_HASH_SIZE; i++) {
        addr_detail_t *curr = l2_table[i];
        while (curr) {
            addr_detail_t *next = curr->next;
            delete curr;
            curr = next;
        }
        l2_table[i] = nullptr;
    }
    
    for (int i = 0; i < NUM_PC_BANKS; i++) {
        pc_banks[i].map.clear();
    }
    
    for (uint32_t i = 0; i < NUM_PC_BANKS; i++) {
        
        pc_banks[i].map.clear();
        
    }
    
    stat_total_ld.store(0, std::memory_order_relaxed);
    stat_total_st.store(0, std::memory_order_relaxed);
    stat_total_push.store(0, std::memory_order_relaxed);
    stat_total_pop.store(0, std::memory_order_relaxed);
    stat_l2_created.store(0, std::memory_order_relaxed);
    stat_l2_drained.store(0, std::memory_order_relaxed);
    
    for (int i = 0; i < 2049; i++) {
        global_lifetime_histogram[i].store(0, std::memory_order_relaxed);
    }

    if (g_cb.mutex_destroy) {
        for (int i = 0; i < NUM_L2_LOCKS; i++) {
            if (l2_locks[i]) { g_cb.mutex_destroy(l2_locks[i]); l2_locks[i] = nullptr; }
        }
        if (dir_mutex) { g_cb.mutex_destroy(dir_mutex); dir_mutex = nullptr; }
        for (int i = 0; i < NUM_PC_BANKS; i++) {
            if (pc_banks[i].lock) { g_cb.mutex_destroy(pc_banks[i].lock); pc_banks[i].lock = nullptr; }
        }
    }

    if (g_cb.mutex_destroy) {
        for (int i = 0; i < NUM_L2_LOCKS; i++) {
            if (l2_locks[i]) { g_cb.mutex_destroy(l2_locks[i]); l2_locks[i] = nullptr; }
        }
        for (int i = 0; i < NUM_PC_BANKS; i++) {
            if (pc_banks[i].lock) { g_cb.mutex_destroy(pc_banks[i].lock); pc_banks[i].lock = nullptr; }
        }
        if (dir_mutex) { g_cb.mutex_destroy(dir_mutex); dir_mutex = nullptr; }
    }
}
