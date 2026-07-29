// macro_engine.h - public surface of the ported ReflexX macro engine.
//
// Corresponds to ReflexX.Application.MacroEngine.MacroProcessor plus the slice
// of InputPipeline that owned macro ordering. Three calls, because that is all
// the firmware needs:
//
//   macro_engine_load()  - called when the active profile changes (boot, or an
//                          on-pad profile-switch combo). Zeroes all runtime
//                          state and caches the priority order, mirroring
//                          "InputPipeline caches macros sorted by priority on
//                          profile load" from the source's conventions.
//   macro_engine_process()- called once per physical XInput report, from
//                          hid_app.c's Phase 4. Mutates the state in place.
//   macro_engine_active_count() - diagnostics for the config channel.
//
// THREADING: the engine is single-threaded by construction. It is only ever
// called from tuh_xinput_report_received_cb on core 1. All runtime state is a
// file-static array in macro_engine.c with no locking, which is safe precisely
// because of that constraint - do not call it from core 0.
#ifndef MACRO_ENGINE_H
#define MACRO_ENGINE_H

#include "macro_types.h"

// Binds a macro array (owned by the caller - normally the active profile in the
// profile store) and resets every runtime state machine. Safe to call at any
// time; the pointer must outlive all subsequent macro_engine_process() calls.
void macro_engine_load(const macro_definition_t *macros, int count);

// Runs every enabled macro, in priority order, against the state in place.
// No-op when no profile is loaded or the profile has no enabled macros, so the
// normal passthrough path costs one branch.
void macro_engine_process(macro_gamepad_state_t *state);

// How many enabled macros the currently-loaded profile has.
int macro_engine_active_count(void);

#endif // MACRO_ENGINE_H
