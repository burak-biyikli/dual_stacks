#include "dr_api.h"
#include <stddef.h>

static void event_exit(void) {}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    dr_set_client_name("Empty Client", "");
    dr_register_exit_event(event_exit);
}
