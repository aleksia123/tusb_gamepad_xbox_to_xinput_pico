// aim_context.h - target-acquisition front-end for the three Tier-3 aim macros
// (AimSnap, AimSmooth, TriggerBot) and for AdaptiveRecoil's distance heuristic.
//
// ═══ WHY THIS IS A STUB, AND WHY THAT IS NOT A CUT CORNER ═══════════════════
// In ReflexX these macros read an AiVisionTelemetrySnapshot published by a
// separate thread that: captures the screen over DXGI, runs a YOLO .onnx model
// on the GPU through DirectML, and tracks the resulting bounding boxes. This
// dongle has no camera, no framebuffer to capture, no GPU and no ~30 MB of
// model weights - the RP2350 has 520 KB of SRAM. The vision pipeline is
// therefore not "unimplemented", it is *unimplementable* on this silicon, and
// the brief scopes it out explicitly.
//
// What we do instead is exactly what the C# source already does when its own
// AimContext is not wired (which is the normal case in its unit tests): every
// one of those handlers opens with `if (_aimContext is null) return state;` or,
// for AdaptiveRecoil, degrades to `distFactor = 0` -> minimum compensation. That
// null-object fallback is the precedent this file follows. Here the equivalent
// is a single acquisition function that always reports "no target":
//
//     bool aim_context_try_acquire(const aim_assist_config_t *, aim_target_t *)
//         -> always false
//
// It is declared __attribute__((weak)) so a future vision coprocessor (an ESP32
// with a camera over UART, say, or a host-side helper feeding targets over the
// config CDC channel) can provide a strong definition in its own translation
// unit and light up all four macros without touching macro_engine.c, the flash
// layout, or the configurator's wire format. Everything downstream of this
// function - the FOV-ring gate, the prediction and ego-motion feed-forward, the
// sticky lock with Schmitt hysteresis, the EMA output smoothing, the plant-
// inverted P controller, the burst timer - is fully ported and live. Only the
// sensor is missing.
//
// The configurator surfaces this honestly: AimSnap/AimSmooth/TriggerBot/
// AdaptiveRecoil appear in the type dropdown with a visible "not available on
// this hardware yet" notice instead of their tuning fields.
#ifndef AIM_CONTEXT_H
#define AIM_CONTEXT_H

#include <math.h>
#include "macro_types.h"

// Mirror of the AI-vision target record the C# AimContext hands back. Kept
// complete (rather than trimmed to what a stub needs) so the struct is the
// integration contract for a future provider: fill these fields and the ported
// math works unchanged.
typedef struct {
    // Bounding box in capture-space pixels, origin top-left.
    float   x, y, width, height;
    // Capture dimensions - the crosshair is assumed at the centre of these.
    float   capture_width, capture_height;
    // FOV acquisition ring radius in pixels (0 -> handlers fall back to 200,
    // matching the overlay default in the C# UI so behaviour matches what the
    // user sees).
    int32_t fov_ring_pixels;
    // Tracker's EMA velocity in px/ms - drives prediction and feed-forward.
    float   velocity_x, velocity_y;
    // px/ms of camera motion per unit of full stick deflection. 0 = uncalibrated,
    // which selects the legacy open-loop pull path in AimSmooth.
    float   calibration_px_per_ms;
    // Identity of the current lock. Bumped only on a genuine target switch, so
    // AimSmooth can retain a lock through fast ego-motion. -1 = none.
    int64_t lock_id;
    // True while the tracker is coasting on a stale box (target not seen this
    // frame). AimSmooth must NOT steer toward a coasting box - see the coast
    // guard in macro_engine.c.
    bool    is_coasting;
} aim_target_t;

// Weak, always-false acquisition. See the header comment for the rationale.
bool aim_context_try_acquire(const aim_assist_config_t *cfg, aim_target_t *out);

// TriggerBot's fire condition: is the crosshair (capture centre) inside the
// locked bbox? Ported from AimContext.IsCrosshairInsideBbox. Pure geometry, so
// it lives here as an inline and stays correct the moment a real provider
// appears.
static inline bool aim_context_crosshair_inside_bbox(const aim_target_t *t)
{
    float cx = t->capture_width  * 0.5f;
    float cy = t->capture_height * 0.5f;
    return cx >= t->x && cx <= t->x + t->width &&
           cy >= t->y && cy <= t->y + t->height;
}

#endif // AIM_CONTEXT_H
