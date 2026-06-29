#include "dr_api.h"
#include <stddef.h> // for NULL
#include <stdint.h>

static void *mutex;
static uint64_t mem_access_count = 0;

/* Clean call invoked for every memory instruction */
static void
at_mem_access(void)
{
    // In the future: Shadow memory lookup goes here
    dr_mutex_lock(mutex);
    mem_access_count++;
    dr_mutex_unlock(mutex);
}

/* Event triggered for every basic block compiled */
static dr_emit_flags_t
event_bb(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
    // Iterate over all instructions in the basic block
    for (instr_t *instr = instrlist_first_app(bb); instr != NULL; instr = instr_get_next_app(instr)) {
        // If the instruction reads or writes memory (LD/ST/PUSH/POP)
        if (instr_reads_memory(instr) || instr_writes_memory(instr)) {
            // Insert a call to our analysis function
            dr_insert_clean_call(drcontext, bb, instr, (void *)at_mem_access, false, 0);
        }
    }
    return DR_EMIT_DEFAULT;
}

/* Event triggered when the application exits */
static void
event_exit(void)
{
    dr_fprintf(STDERR, "[Custom DR Analyzer] Total memory instructions executed: %llu\n", mem_access_count);
    dr_mutex_destroy(mutex);
}

/* Initialization */
DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    dr_set_client_name("Memory Access Counter", "http://dynamorio.org/issues");
    mutex = dr_mutex_create();
    
    // Register events
    dr_register_exit_event(event_exit);
    dr_register_bb_event(event_bb);
    
    dr_fprintf(STDERR, "[Custom DR Analyzer] Initialized! Tracking LD/ST...\n");
}
