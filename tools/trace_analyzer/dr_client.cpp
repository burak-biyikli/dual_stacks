#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"
#include "analyzer.h"
#include <string.h>
#include <stddef.h>

static int tls_idx;

typedef struct {
    uint8_t tid;
    uintptr_t ghr;
} per_thread_t;

static uint8_t next_tid = 1;
static void *tid_mutex;

static uint32_t global_strict_ipc_push[MAX_TID + 1];
static uint32_t global_strict_ipc_pop[MAX_TID + 1];
static void *ipc_mutex;


static void* cb_mutex_create() {
    return dr_mutex_create();
}
static void cb_mutex_destroy(void* m) {
    dr_mutex_destroy(m);
}
static void cb_mutex_lock(void* m) {
    dr_mutex_lock(m);
}
static void cb_mutex_unlock(void* m) {
    dr_mutex_unlock(m);
}

static bool cb_mutex_trylock(void* m) {
    return dr_mutex_trylock(m);
}

static void cb_add_strict_ipc_impl(uint8_t tid, uint32_t pushes, uint32_t pops) {
    dr_mutex_lock(ipc_mutex);
    global_strict_ipc_push[tid] += pushes;
    global_strict_ipc_pop[tid] += pops;
    dr_mutex_unlock(ipc_mutex);
}

static void cb_log_debug(const char *msg) {
    dr_fprintf(STDERR, "[Analyzer Debug] %s\n", msg);
}

static inline uint8_t get_ctx_hash(uintptr_t ghr);

static void clean_call_push(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_push(pt->tid, (uintptr_t)addr, size, (uintptr_t)pc, get_ctx_hash(pt->ghr));
}

static void clean_call_pop(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_pop(pt->tid, (uintptr_t)addr, size, (uintptr_t)pc, get_ctx_hash(pt->ghr));
}

static void clean_call_ld(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_ld(pt->tid, (uintptr_t)addr, size);
}

static void clean_call_st(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_st(pt->tid, (uintptr_t)addr, size);
}

static void clean_call_add_clock(uint32_t ticks) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_add_logical_clock(pt->tid, ticks);
}


#define GHR_SHIFT_AMOUNT 22
#define CTX_HASH_BITS 3
#define CTX_BUCKETS (1 << CTX_HASH_BITS)

static inline uint8_t get_ctx_hash(uintptr_t ghr) {
    // MurmurHash3 64-bit finalizer
    ghr ^= ghr >> 33;
    ghr *= 0xff51afd7ed558ccdULL;
    ghr ^= ghr >> 33;
    ghr *= 0xc4ceb9fe1a85ec53ULL;
    ghr ^= ghr >> 33;
    return (uint8_t)(ghr & (CTX_BUCKETS - 1));
}

static void clean_call_bb_entry(app_pc bb_pc) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    pt->ghr = (pt->ghr << GHR_SHIFT_AMOUNT) ^ (uintptr_t)bb_pc;
}

static dr_emit_flags_t event_app_analysis(void *drcontext, void *tag, instrlist_t *bb,
                                          bool for_trace, bool translating,
                                          void **user_data) {
    uint32_t reg_writes = 0;
    for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr)) {
        for (int i = 0; i < instr_num_dsts(instr); i++) {
            if (opnd_is_reg(instr_get_dst(instr, i))) {
                reg_writes++;
                break;
            }
        }
    }
    *user_data = (void *)(uintptr_t)reg_writes;
    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag, instrlist_t *bb,
                                             instr_t *instr, bool for_trace,
                                             bool translating, void *user_data) {

    if (drmgr_is_first_instr(drcontext, instr)) {
        dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call_bb_entry, false, 1, OPND_CREATE_INTPTR(tag));

        uint32_t reg_writes = (uint32_t)(uintptr_t)user_data;
        if (reg_writes > 0) {
            dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call_add_clock, false, 1, OPND_CREATE_INT32(reg_writes));
        }
    }
    
    if (!instr_is_app(instr)) return DR_EMIT_DEFAULT;
    
    bool is_mem = instr_reads_memory(instr) || instr_writes_memory(instr);
    if (!is_mem) return DR_EMIT_DEFAULT;
    
    app_pc pc = instr_get_app_pc(instr);
    int opcode = instr_get_opcode(instr);
    
    bool is_push = (opcode == OP_push || opcode == OP_pusha || opcode == OP_pushf);
    bool is_pop  = (opcode == OP_pop || opcode == OP_popa || opcode == OP_popf);
    
    for (int i = 0; i < instr_num_srcs(instr); i++) {
        opnd_t op = instr_get_src(instr, i);
        if (opnd_is_memory_reference(op)) {
            uint32_t size = opnd_size_in_bytes(opnd_get_size(op));
            if (size == 0) size = 8;
            
            reg_id_t reg1, reg2;
            if (drreg_reserve_register(drcontext, bb, instr, NULL, &reg1) == DRREG_SUCCESS &&
                drreg_reserve_register(drcontext, bb, instr, NULL, &reg2) == DRREG_SUCCESS) {
                
                drutil_insert_get_mem_addr(drcontext, bb, instr, op, reg1, reg2);
                dr_insert_clean_call(drcontext, bb, instr,
                                     (void *)(is_pop ? clean_call_pop : clean_call_ld),
                                     false, 3,
                                     OPND_CREATE_INTPTR(pc),
                                     opnd_create_reg(reg1),
                                     OPND_CREATE_INT32(size));
                
                drreg_unreserve_register(drcontext, bb, instr, reg2);
                drreg_unreserve_register(drcontext, bb, instr, reg1);
            }
        }
    }
    
    for (int i = 0; i < instr_num_dsts(instr); i++) {
        opnd_t op = instr_get_dst(instr, i);
        if (opnd_is_memory_reference(op)) {
            uint32_t size = opnd_size_in_bytes(opnd_get_size(op));
            if (size == 0) size = 8;
            
            reg_id_t reg1, reg2;
            if (drreg_reserve_register(drcontext, bb, instr, NULL, &reg1) == DRREG_SUCCESS &&
                drreg_reserve_register(drcontext, bb, instr, NULL, &reg2) == DRREG_SUCCESS) {
                
                drutil_insert_get_mem_addr(drcontext, bb, instr, op, reg1, reg2);
                dr_insert_clean_call(drcontext, bb, instr,
                                     (void *)(is_push ? clean_call_push : clean_call_st),
                                     false, 3,
                                     OPND_CREATE_INTPTR(pc),
                                     opnd_create_reg(reg1),
                                     OPND_CREATE_INT32(size));
                
                drreg_unreserve_register(drcontext, bb, instr, reg2);
                drreg_unreserve_register(drcontext, bb, instr, reg1);
            }
        }
    }
    
    return DR_EMIT_DEFAULT;
}

static void event_thread_init(void *drcontext) {
    per_thread_t *pt = (per_thread_t *)dr_thread_alloc(drcontext, sizeof(per_thread_t));
    
    dr_mutex_lock(tid_mutex);
    pt->tid = next_tid++;
    if (pt->tid > MAX_TID) pt->tid = MAX_TID;
    dr_mutex_unlock(tid_mutex);
    
    drmgr_set_tls_field(drcontext, tls_idx, pt);
    
    dr_mcontext_t mc = {sizeof(mc), DR_MC_CONTROL};
    dr_get_mcontext(drcontext, &mc);
    
    uintptr_t stack_base = (uintptr_t)mc.xsp - (8 * 1024 * 1024);
    uintptr_t stack_top  = (uintptr_t)mc.xsp + (1 * 1024 * 1024);
    
    analyzer_register_thread(pt->tid, stack_base, stack_top);
}

static void event_thread_exit(void *drcontext) {
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_unregister_thread(pt->tid);
    dr_thread_free(drcontext, pt, sizeof(per_thread_t));
}

static void dump_stats(bool abnormal_exit) {
    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    
    uint64_t sum_possible_push = 0;
    uint64_t sum_possible_pop = 0;
    uint32_t unique_pcs = 0;
    uint32_t unique_ipc_pcs = 0;
    uint64_t hist_sum_push = 0;
    uint64_t hist_sum_pop = 0;
    
    analyzer_get_pc_stats(&sum_possible_push, &sum_possible_pop, &hist_sum_push, &hist_sum_pop, &unique_pcs, &unique_ipc_pcs, abnormal_exit);
    
    uint32_t total_strict_push = 0;
    uint32_t total_strict_pop = 0;
    for (int i = 1; i <= MAX_TID; i++) {
        total_strict_push += global_strict_ipc_push[i];
        total_strict_pop += global_strict_ipc_pop[i];
    }
    
    uint64_t final_possible_push = (sum_possible_push > total_strict_push) ? sum_possible_push - total_strict_push : 0;
    uint64_t final_possible_pop  = (sum_possible_pop > total_strict_pop) ? sum_possible_pop - total_strict_pop : 0;
    
    uint64_t hist_final_push = (hist_sum_push > total_strict_push) ? hist_sum_push - total_strict_push : 0;
    uint64_t hist_final_pop  = (hist_sum_pop > total_strict_pop) ? hist_sum_pop - total_strict_pop : 0;
    
    uint64_t total_mem = stats.total_ld + stats.total_st + stats.total_push + stats.total_pop;
    uint64_t total_stack = stats.total_push + stats.total_pop;
    
    dr_fprintf(STDERR, "=== Stack Privacy Analysis ===\n");
    dr_fprintf(STDERR, "[APP: %s]\n", dr_get_application_name());
    dr_fprintf(STDERR, "Total Memory Operations: %llu\n", total_mem);
    dr_fprintf(STDERR, "  Loads:  %llu\n", stats.total_ld);
    dr_fprintf(STDERR, "  Stores: %llu\n", stats.total_st);
    dr_fprintf(STDERR, "  Pushes: %llu\n", stats.total_push);
    dr_fprintf(STDERR, "  Pops:   %llu\n", stats.total_pop);
    dr_fprintf(STDERR, "\nStack Operations (PUSH+POP): %llu\n", total_stack);
    
    uint64_t provably_private = total_stack - total_strict_push - total_strict_pop - final_possible_push - final_possible_pop;
    dr_fprintf(STDERR, "  Provably Private:                        %llu\n", provably_private);
    dr_fprintf(STDERR, "  Strictly IPC (Instance Level):           %u\n", total_strict_push + total_strict_pop);
    dr_fprintf(STDERR, "  Possibly IPC (Non-History):              %llu\n", final_possible_push + final_possible_pop);
    dr_fprintf(STDERR, "  Possibly IPC (History-Based, %d buckets): %llu\n", CTX_BUCKETS, hist_final_push + hist_final_pop);
    dr_fprintf(STDERR, "\nUnique Raw Stack PCs:          %u\n", unique_pcs);
    dr_fprintf(STDERR, "Unique Raw Stack PCs with IPC: %u\n", unique_ipc_pcs);
    
    if (!abnormal_exit) analyzer_drain_hanging_pushes();
    
    dr_fprintf(STDERR, "\n=== Stack Lifetime Histogram (Register Assignments) ===\n");
    const uint64_t* histo = analyzer_get_histogram();
    for (int i = 0; i <= 2048; i++) {
        if (histo[i] > 0) {
            if (i == 2048) dr_fprintf(STDERR, "  [> 2047]: %llu\n", histo[i]);
            else           dr_fprintf(STDERR, "  [%d]: %llu\n", i, histo[i]);
        }
    }
    
    if (!abnormal_exit) {
        dr_mutex_destroy(tid_mutex);
        dr_mutex_destroy(ipc_mutex);
        
        drmgr_unregister_tls_field(tls_idx);
        drmgr_unregister_thread_init_event(event_thread_init);
        drmgr_unregister_thread_exit_event(event_thread_exit);
        drmgr_unregister_bb_insertion_event(event_app_instruction);
        
        drutil_exit();
        drreg_exit();
        drmgr_exit();
        
        analyzer_cleanup();
    }
}

static bool g_abnormal_exit_triggered = false;

static void event_exit(void) {
    if (g_abnormal_exit_triggered) return;
    dump_stats(false);
}

static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
    if (info->sig == 15 /* SIGTERM */ || info->sig == 2 /* SIGINT */) {
        if (!g_abnormal_exit_triggered) {
            g_abnormal_exit_triggered = true;
            dr_fprintf(STDERR, "Received termination signal %d. Dumping stats...\n", info->sig);
            dump_stats(true);
        }
        return DR_SIGNAL_DELIVER;
    }
    return DR_SIGNAL_DELIVER;
}

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static void client_thread_func(void *param) {
    const char *fifo_path = (const char *)param;
    int fd = open(fifo_path, O_RDWR);
    if (fd >= 0) {
        char buf[1];
        if (read(fd, buf, 1) > 0) {
            if (!g_abnormal_exit_triggered) {
                g_abnormal_exit_triggered = true;
                dr_fprintf(STDERR, "Received timeout signal via FIFO. Dumping stats...\n");
                dump_stats(true);
                dr_abort_with_code(1);
            }
        }
        close(fd);
    }
    dr_global_free((void*)fifo_path, strlen(fifo_path) + 1);
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    dr_set_client_name("DynamoRIO Stack Privacy Analyzer", "http://dynamorio.org/issues");
    
    const char *fifo_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fifo_path") == 0 && i + 1 < argc) {
            fifo_path = argv[i+1];
            i++;
        }
    }
    
    if (fifo_path != NULL) {
        char *path_copy = (char *)dr_global_alloc(strlen(fifo_path) + 1);
        strcpy(path_copy, fifo_path);
        dr_create_client_thread(client_thread_func, path_copy);
    }
    
    drmgr_init();
    
    drmgr_register_signal_event(event_signal);
    
    drreg_options_t ops = {sizeof(ops), 3, false};
    drreg_init(&ops);
    drutil_init();
    
    tls_idx = drmgr_register_tls_field();
    
    tid_mutex = dr_mutex_create();
    ipc_mutex = dr_mutex_create();
    
    memset(global_strict_ipc_push, 0, sizeof(global_strict_ipc_push));
    memset(global_strict_ipc_pop, 0, sizeof(global_strict_ipc_pop));
    
    analyzer_callbacks_t cb = {};
    cb.mutex_create = cb_mutex_create;
    cb.mutex_destroy = cb_mutex_destroy;
    cb.mutex_lock = cb_mutex_lock;
    cb.mutex_unlock = cb_mutex_unlock;
    cb.mutex_trylock = cb_mutex_trylock;
    cb.add_strict_ipc = cb_add_strict_ipc_impl;
    cb.log_debug = cb_log_debug;
    
    analyzer_init(&cb);
    
    drmgr_register_thread_init_event(event_thread_init);
    drmgr_register_thread_exit_event(event_thread_exit);
    drmgr_register_bb_instrumentation_event(event_app_analysis, event_app_instruction, NULL);
    drmgr_register_exit_event(event_exit);
}
