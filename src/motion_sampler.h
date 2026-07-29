// motion_sampler.h - port of ReflexX.Application.MacroEngine.MotionSampler.
//
// A pure function of (motion_script_t, elapsed_ms) -> normalised (x, y) in
// [-1, 1] plus a completion flag. Header-only and side-effect free for the same
// reason the C# original was a static class with "zero allocations on the hot
// path and no thread-local state": it is called from the XInput report callback
// on core 1, possibly several times per tick (ScriptedShape and HeadAssist both
// use it), so it must be inlinable and must never touch shared state.
//
// The one substantive change from the source is double -> float (see the
// tradeoff note in mono_clock.h). Everything downstream multiplies these values
// by 32767 and rounds to int16, so float's ~7 digits are far more than the
// output can carry.
#ifndef MOTION_SAMPLER_H
#define MOTION_SAMPLER_H

#include <math.h>
#include "macro_types.h"

#ifndef M_PIf
#define M_PIf 3.14159265358979323846f
#endif
#define DEG2RADf (M_PIf / 180.0f)

typedef struct {
    float x;
    float y;
    bool  completed;
} motion_sample_t;

// EaseOutBack's overshoot constants, straight from the source.
static inline float ms_ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

static inline float ms_ease(uint8_t kind, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    switch (kind) {
        case EASE_LINEAR:       return t;
        case EASE_OUT_QUAD:     return 1.0f - (1.0f - t) * (1.0f - t);
        case EASE_OUT_CUBIC:    return 1.0f - powf(1.0f - t, 3.0f);
        case EASE_IN_OUT_SINE:  return 0.5f - 0.5f * cosf(M_PIf * t);
        case EASE_OUT_BACK:     return ms_ease_out_back(t);
        case EASE_SMOOTHSTEP:   return t * t * (3.0f - 2.0f * t);
        default:                return t;
    }
}

static inline float ms_smoothstep(float t)
{
    float c = clampf(t, 0.0f, 1.0f);
    return c * c * (3.0f - 2.0f * c);
}

static inline float ms_phase_radians(const motion_script_t *s, float elapsed_ms)
{
    float sign        = s->clockwise ? 1.0f : -1.0f;
    float cycle_phase = (elapsed_ms / s->period_ms) * 2.0f * M_PIf;
    float start_phase = s->start_phase_deg * DEG2RADf;
    return start_phase + sign * cycle_phase;
}

// ── Flick - one-shot directional pulse with easing on amplitude ──────────────
// Note the (1 - progress) argument to the easing curve: amplitude PEAKS at t=0
// and decays, because that is how a real flick feels - the stick snaps out and
// the spring pulls it back. Getting this backwards inverts the whole feel, so it
// is preserved verbatim.
static inline motion_sample_t ms_sample_flick(const motion_script_t *s, float elapsed_ms)
{
    motion_sample_t out = {0.0f, 0.0f, true};
    if (s->duration_ms <= 0.0f) return out;

    float progress = clampf(elapsed_ms / s->duration_ms, 0.0f, 1.0f);
    out.completed  = progress >= 1.0f;

    float amp = s->amplitude_norm * s->intensity_mul * ms_ease(s->easing, 1.0f - progress);
    float rad = s->direction_deg * DEG2RADf;
    out.x = amp * cosf(rad);
    out.y = amp * sinf(rad);
    return out;
}

// ── Circle - constant radius, constant angular velocity ─────────────────────
static inline motion_sample_t ms_sample_circle(const motion_script_t *s, float elapsed_ms)
{
    motion_sample_t out = {0.0f, 0.0f, true};
    if (s->period_ms <= 0.0f) return out;

    float theta = ms_phase_radians(s, elapsed_ms);
    float r     = s->radius_x_norm * s->intensity_mul;
    out.x = r * cosf(theta);
    out.y = r * sinf(theta);
    out.completed = (s->duration_ms > 0.0f) && (elapsed_ms >= s->duration_ms);
    return out;
}

// ── Ellipse - independent semi-axes, optional rotation ──────────────────────
static inline motion_sample_t ms_sample_oval(const motion_script_t *s, float elapsed_ms,
                                             float rx, float ry, float rotation_deg)
{
    motion_sample_t out = {0.0f, 0.0f, true};
    if (s->period_ms <= 0.0f) return out;

    float theta = ms_phase_radians(s, elapsed_ms);
    float lx = rx * s->intensity_mul * cosf(theta);
    float ly = ry * s->intensity_mul * sinf(theta);

    float rot = rotation_deg * DEG2RADf;
    float c = cosf(rot), sn = sinf(rot);
    out.x = lx * c - ly * sn;
    out.y = lx * sn + ly * c;
    out.completed = (s->duration_ms > 0.0f) && (elapsed_ms >= s->duration_ms);
    return out;
}

// VerticalOval swaps rx/ry and adds the fixed 90 deg rotation, exactly as the
// C# Evaluate() switch does - that is what makes it read as "tall" rather than
// merely "wide, relabelled".
static inline motion_sample_t motion_sampler_evaluate(const motion_script_t *s, float elapsed_ms)
{
    switch (s->shape) {
        case SHAPE_FLICK:           return ms_sample_flick(s, elapsed_ms);
        case SHAPE_CIRCLE:          return ms_sample_circle(s, elapsed_ms);
        case SHAPE_HORIZONTAL_OVAL: return ms_sample_oval(s, elapsed_ms, s->radius_x_norm, s->radius_y_norm, 0.0f);
        case SHAPE_VERTICAL_OVAL:   return ms_sample_oval(s, elapsed_ms, s->radius_y_norm, s->radius_x_norm, 90.0f);
        case SHAPE_DIAGONAL_OVAL:   return ms_sample_oval(s, elapsed_ms, s->radius_x_norm, s->radius_y_norm, s->rotation_deg);
        default: {
            motion_sample_t out = {0.0f, 0.0f, true};
            return out;
        }
    }
}

#endif // MOTION_SAMPLER_H
