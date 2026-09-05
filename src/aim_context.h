#ifndef AIM_CONTEXT_H
#define AIM_CONTEXT_H

#include <math.h>
#include "macro_types.h"

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


bool aim_context_try_acquire(const aim_assist_config_t *cfg, aim_target_t *out);

// Is the crosshair (capture centre) inside the
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
