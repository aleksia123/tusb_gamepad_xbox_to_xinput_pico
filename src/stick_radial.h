// stick_radial.h - right-stick radial stick-gate shaping. Two INDEPENDENT stages:
//   (1) per-axis gain + hard unit-circle correction  [gated by uncap_radius]
//   (2) soft corner cap                               [always runs, own knob]
// Left stick is passthrough only and does not go through this header at all.
//
// --- Stage 1: gain + unit-circle correction (uncap_radius) ---
// Background: for a stick whose X/Y sensors reach different physical maxima,
// a plain unit-circle clamp hard-clips the diagonals but leaves the cardinals
// under-scaled - the trace comes out elliptical. Stage 1 fixes that with
// per-axis gain calibration (scale each axis to its own observed max) before
// a hard unit-circle clamp, producing a CIRCULAR trace (0% overshoot).
//   uncap_radius == true  -> Stage 1 skipped entirely (raw overshoot kept).
//   uncap_radius == false -> per-axis gain + hard circle. Circular trace.
// NOTE: on the R2P this stage is deliberately OFF. Measured avg circularity
// error is ~8% (healthy stock range), so forcing a circle would over-correct
// and cost diagonal reach. See pad_config.h.
//
// --- Stage 2: soft corner cap (right_stick_corner_cap_pct) ---
// Runs REGARDLESS of uncap_radius, so you can keep the natural diagonal
// overshoot but bound its PEAK. It trims only the vector tip that exceeds
// (cap_pct/100)*32767; anything shorter passes untouched. Because it touches
// only the sharpest corner and leaves the rest of the perimeter alone, it
// pulls peak corner reach down (e.g. +31.8% -> +20%) while the tester's
// AVG circularity error stays ~unchanged (~8%). cap_pct: 0=off, 100=hard
// circle, 101..255 = soft cap at that % of full scale.
//
// Order in hid_app.c: axial deadzone (pre-clean) -> correct_right_stick.
// Stage 1 then Stage 2 both live inside correct_right_stick below.
#ifndef STICK_RADIAL_H
#define STICK_RADIAL_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// ── Runtime calibration state (right stick only) ────────────────────────
// Declare one instance as a file-scope static in hid_app.c:
//   static RightStickCal rs_cal = {0};
typedef struct {
    float    max_x;      // largest |rx| seen, in float
    float    max_y;      // largest |ry| seen
    uint32_t frames;     // frames observed since boot
    bool     calibrated; // true once max_x / max_y are considered stable
} RightStickCal;

#define CALIB_FRAMES          600u      // ~10s at 60Hz before correction activates
#define MIN_CALIB_THRESHOLD   16384.0f  // stick must reach half-travel to count

// Call every frame. Gated on uncap_radius: in passthrough mode calibration
// freezes (frames, maxes, and the calibrated flag all hold), so it only
// converges while Stage 1 correction is active. Pass the same uncap_radius
// flag you pass to correct_right_stick.
static inline void rs_cal_update(RightStickCal* cal, int16_t rx, int16_t ry,
                                 bool uncap_radius) {
    if (uncap_radius) return; // Stage 1 off -> freeze calibration

    float ax = fabsf((float)rx);
    float ay = fabsf((float)ry);

    if (ax > cal->max_x) cal->max_x = ax;
    if (ay > cal->max_y) cal->max_y = ay;

    if (cal->frames < CALIB_FRAMES) cal->frames++;

    if (!cal->calibrated && cal->frames >= CALIB_FRAMES) {
        if (cal->max_x > MIN_CALIB_THRESHOLD && cal->max_y > MIN_CALIB_THRESHOLD) {
            cal->calibrated = true;
        }
    }
}

// Right stick shaping.
//   Stage 1 (uncap_radius == false): per-axis gain + hard unit-circle clamp.
//   Stage 2 (corner_cap_pct != 0):   soft cap at (pct/100)*32767, ALWAYS runs.
// The two are independent: Stage 1 can be off (uncap=true) while Stage 2 caps
// the corner. Pass cfg->right_stick_corner_cap_pct as corner_cap_pct.
static inline void correct_right_stick(int16_t* x, int16_t* y,
                                        const RightStickCal* cal,
                                        bool uncap_radius,
                                        uint8_t corner_cap_pct) {
    float fx = (float)*x;
    float fy = (float)*y;

    // ---- Stage 1: gain + hard unit circle (only when uncap_radius == false) ----
    if (!uncap_radius) {
        if (cal->calibrated) {
            fx *= 32767.0f / cal->max_x;
            fy *= 32767.0f / cal->max_y;
        }
        float mag = sqrtf(fx * fx + fy * fy);
        if (mag > 32767.0f) {
            float scale = 32767.0f / mag;
            fx *= scale;
            fy *= scale;
        }
    }

    // ---- Stage 2: soft corner cap (independent; runs whenever pct != 0) ----
    if (corner_cap_pct != 0) {
        float cap = 32767.0f * ((float)corner_cap_pct / 100.0f);
        float mag = sqrtf(fx * fx + fy * fy);
        if (mag > cap) {
            float scale = cap / mag;
            fx *= scale;
            fy *= scale;
        }
    }

    // ---- round + saturate to int16 ----
    int32_t ox = (int32_t)(fx + (fx >= 0.0f ? 0.5f : -0.5f));
    int32_t oy = (int32_t)(fy + (fy >= 0.0f ? 0.5f : -0.5f));

    if (ox < -32768) ox = -32768;
    if (ox >  32767) ox = 32767;
    if (oy < -32768) oy = -32768;
    if (oy >  32767) oy =  32767;

    *x = (int16_t)ox;
    *y = (int16_t)oy;
}

#endif // STICK_RADIAL_H
