#include "aim_context.h"

__attribute__((weak))
bool aim_context_try_acquire(const aim_assist_config_t *cfg, aim_target_t *out)
{
    (void)cfg;

    if (out) {
        aim_target_t empty = {0};
        empty.lock_id = -1;
        *out = empty;
    }
    return false; // no vision pipeline on this hardware
}
