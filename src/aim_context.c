// aim_context.c - the deliberate no-target stub. See aim_context.h for the full
// rationale; the short version is that this hardware has no camera, and the C#
// source's own null-AimContext path is the behaviour being reproduced.
//
// Declared weak so a future target provider can override it by simply linking a
// strong definition of the same symbol. Nothing else in the firmware needs to
// change for that to work.
#include "aim_context.h"

__attribute__((weak))
bool aim_context_try_acquire(const aim_assist_config_t *cfg, aim_target_t *out)
{
    (void)cfg;
    // Zero the record anyway: a provider that returns false must never leave a
    // caller reading uninitialised stack, and the ported handlers all check the
    // bool before touching *out, so this is belt-and-braces.
    if (out) {
        aim_target_t empty = {0};
        empty.lock_id = -1;
        *out = empty;
    }
    return false; // no vision pipeline on this hardware
}
