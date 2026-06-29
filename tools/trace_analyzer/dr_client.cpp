#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drutil.h"
#include "hashtable.h"
#include "analyzer.h"
#include <string.h>
#include <stddef.h>

static int tls_idx;
static hashtable_t pc_table;
static void *pc_table_mutex;

typedef struct {
    uint8_t tid;
} per_thread_t;

typedef struct {
    uint64_t total_count;
    bool is_ipc;
    bool is_push;
} pc_global_t;

static uint8_t next_tid = 1;
static void *tid_mutex;

static void cb_mark_pc_ipc(uintptr_t pc, bool is_push) {
    dr_mutex_lock(pc_table_mutex);
    pc_global_t *g = (pc_global_t *)hashtable_lookup(&pc_table, (void*)pc);
    if (g) {
        g->is_ipc = true;
    }
    dr_mutex_unlock(pc_table_mutex);
}

static uint32_t global_strict_ipc_push[MAX_TID + 1];
static uint32_t global_strict_ipc_pop[MAX_TID + 1];
static void *ipc_mutex;

static void cb_add_strict_ipc_impl(uint8_t tid, uint32_t pushes, uint32_t pops) {
    dr_mutex_lock(ipc_mutex);
    global_strict_ipc_push[tid] += pushes;
    global_strict_ipc_pop[tid] += pops;
    dr_mutex_unlock(ipc_mutex);
}

static void cb_log_debug(const char *msg) {
    dr_fprintf(STDERR, "[Analyzer Debug] %s\n", msg);
}

static void clean_call_push(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_push(pt->tid, (uintptr_t)addr, size, (uintptr_t)pc);
}

static void clean_call_pop(app_pc pc, app_pc addr, uint32_t size) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_on_pop(pt->tid, (uintptr_t)addr, size, (uintptr_t)pc);
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

static void clean_call_inc_pc(app_pc pc) {
    dr_mutex_lock(pc_table_mutex);
    pc_global_t *g = (pc_global_t *)hashtable_lookup(&pc_table, (void*)pc);
    if (g) {
        g->total_count++;
    }
    dr_mutex_unlock(pc_table_mutex);
}

static void clean_call_add_clock(uint32_t ticks) {
    void *drcontext = dr_get_current_drcontext();
    per_thread_t *pt = (per_thread_t *)drmgr_get_tls_field(drcontext, tls_idx);
    analyzer_update_clock(pt->tid, ticks);
}

static dr_emit_flags_t event_app_analysis(void *drcontext, void *tag, instrlist_t *bb,
                                          bool for_trace, bool translating,
                                          void **user_data) {
    uint32_t reg_writes = 0;
    for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr)) {
        for (int i = 0; i < instr_num_dsts(instr); i++) {
            if (opnd_is_reg(instr_get_dst(instr, i))) {
                reg_writes++;
                break; // 1 assignment per instruction max to count instructions doing reg assignment
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
    
    dr_mutex_lock(pc_table_mutex);
    pc_global_t *g = (pc_global_t *)hashtable_lookup(&pc_table, (void*)pc);
    if (!g) {
        g = (pc_global_t *)dr_global_alloc(sizeof(pc_global_t));
        g->total_count = 0;
        g->is_ipc = false;
        g->is_push = is_push;
        hashtable_add(&pc_table, (void*)pc, g);
    }
    dr_mutex_unlock(pc_table_mutex);
    
    dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call_inc_pc, false, 1, OPND_CREATE_INTPTR(pc));

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

static void free_pc_payload(void *payload) {
    dr_global_free(payload, sizeof(pc_global_t));
}

static void event_exit(void) {
    analyzer_stats_t stats;
    analyzer_get_stats(&stats);
    
    uint64_t sum_possible_push = 0;
    uint64_t sum_possible_pop = 0;
    uint32_t unique_pcs = 0;
    uint32_t unique_ipc_pcs = 0;
    
    for (uint i = 0; i < HASHTABLE_SIZE(pc_table.table_bits); i++) {
        hash_entry_t *e = pc_table.table[i];
        while (e) {
            pc_global_t *g = (pc_global_t *)e->payload;
            unique_pcs++;
            if (g->is_ipc) {
                unique_ipc_pcs++;
                if (g->is_push) sum_possible_push += g->total_count;
                else            sum_possible_pop += g->total_count;
            }
            e = e->next;
        }
    }
    
    uint32_t total_strict_push = 0;
    uint32_t total_strict_pop = 0;
    for (int i = 1; i <= MAX_TID; i++) {
        total_strict_push += global_strict_ipc_push[i];
        total_strict_pop += global_strict_ipc_pop[i];
    }
    
    uint64_t final_possible_push = (sum_possible_push > total_strict_push) ? sum_possible_push - total_strict_push : 0;
    uint64_t final_possible_pop  = (sum_possible_pop > total_strict_pop) ? sum_possible_pop - total_strict_pop : 0;
    
    uint64_t total_mem = stats.total_ld + stats.total_st + stats.total_push + stats.total_pop;
    uint64_t total_stack = stats.total_push + stats.total_pop;
    
    dr_fprintf(STDERR, "=== Stack Privacy Analysis ===\n");
    dr_fprintf(STDERR, "Total Memory Operations: %llu\n", total_mem);
    dr_fprintf(STDERR, "  Loads:  %llu\n", stats.total_ld);
    dr_fprintf(STDERR, "  Stores: %llu\n", stats.total_st);
    dr_fprintf(STDERR, "  Pushes: %llu\n", stats.total_push);
    dr_fprintf(STDERR, "  Pops:   %llu\n", stats.total_pop);
    dr_fprintf(STDERR, "\nStack Operations (PUSH+POP): %llu\n", total_stack);
    
    uint64_t provably_private = total_stack - total_strict_push - total_strict_pop - final_possible_push - final_possible_pop;
    dr_fprintf(STDERR, "  Provably Private:              %llu\n", provably_private);
    dr_fprintf(STDERR, "  Strictly IPC (Instance Level): %u\n", total_strict_push + total_strict_pop);
    dr_fprintf(STDERR, "  Possibly IPC (PC Level):       %llu\n", final_possible_push + final_possible_pop);
    dr_fprintf(STDERR, "\nUnique Stack PCs:          %u\n", unique_pcs);
    dr_fprintf(STDERR, "Unique Stack PCs with IPC: %u\n", unique_ipc_pcs);
    
    dr_fprintf(STDERR, "\n=== Stack Lifetime Histogram (Register Assignments) ===\n");
    const uint64_t* histo = analyzer_get_histogram();
    for (int i = 0; i <= 2048; i++) {
        if (histo[i] > 0) {
            if (i == 2048) dr_fprintf(STDERR, "  [> 2047]: %llu\n", histo[i]);
            else           dr_fprintf(STDERR, "  [%d]: %llu\n", i, histo[i]);
        }
    }
    
    hashtable_delete(&pc_table);
    dr_mutex_destroy(pc_table_mutex);
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

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    dr_set_client_name("DynamoRIO Stack Privacy Analyzer", "http://dynamorio.org/issues");
    
    drmgr_init();
    drreg_options_t ops = {sizeof(ops), 3, false};
    drreg_init(&ops);
    drutil_init();
    
    tls_idx = drmgr_register_tls_field();
    
    pc_table_mutex = dr_mutex_create();
    tid_mutex = dr_mutex_create();
    ipc_mutex = dr_mutex_create();
    
    hashtable_init_ex(&pc_table, 16, HASH_INTPTR, false, false, free_pc_payload, NULL, NULL);
    
    memset(global_strict_ipc_push, 0, sizeof(global_strict_ipc_push));
    memset(global_strict_ipc_pop, 0, sizeof(global_strict_ipc_pop));
    
    analyzer_callbacks_t cb = {
        .mark_pc_ipc = cb_mark_pc_ipc,
        .add_strict_ipc = cb_add_strict_ipc_impl,
        .log_debug = cb_log_debug,
        .mutex_create = dr_mutex_create,
        .mutex_lock = dr_mutex_lock,
        .mutex_unlock = dr_mutex_unlock,
        .mutex_destroy = dr_mutex_destroy
    };
    analyzer_init(cb);
    
    drmgr_register_thread_init_event(event_thread_init);
    drmgr_register_thread_exit_event(event_thread_exit);
    drmgr_register_bb_instrumentation_event(event_app_analysis, event_app_instruction, NULL);
    drmgr_register_exit_event(event_exit);
}
