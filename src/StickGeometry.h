// StickGeometry.h — portable analog-stick geometry correction.
//
// Header-only, C++17, float math (RP2350 has a hardware FPU; on RP2040 build
// with -DSG_USE_FLOAT=0 is NOT supported — port to fix16 if you need it).
// No dependencies. The same object code runs in firmware and on the desktop,
// so a correction A/B'd in the bridge is the correction that ships.
//
// STAGE ORDER — this is the whole design decision, everything else is detail:
//
//   int16 in
//     1. normalise            symmetric int16 -> [-1,1]
//     2. centre offset        remove measured resting centre
//     3. Y bias removal       subtract the measured Y offset vs |x|
//     4. per-direction gain   independent scale for -X/+X/-Y/+Y
//     5. angular normalise    r /= rmax(theta)   (optional LUT)
//     6. circle clamp         r <= 1
//     7. axial deadzone       per-axis, scaled or hard
//     8. output ceiling       radius `out_scale` maps to full output
//     9. gate map             circle <-> square domain conversion
//    10. quantise             integer shift, LAST
//   int16 out
//
// Rationale for the order:
//   - centre before gain, or the gain asymmetry absorbs the offset
//   - gain and angular normalisation before any clamp, or you clamp against
//     an uncorrected radius
//   - deadzone after normalisation so the band is a fixed fraction of the
//     CORRECTED range, not of whatever that direction happened to reach
//   - quantise last so no later stage can de-grid the output
//   - the gate map goes AFTER the output ceiling. Every earlier stage assumes
//     a circular domain; the ceiling is a RADIAL clamp, so applying it after
//     the square map would crush the diagonals straight back to a circle and
//     undo the mapping entirely.
//
//   - Y bias removal runs on the RAW signal, right after the centre offset and
//     before any geometry normalisation. It corrects a sensor defect; the
//     later stages fit the gate envelope, and they should be fitting the
//     envelope of an already-corrected signal.
//
// Stage 3 removes a measured Y offset that appears as a function of |x| --
// NOT cross-axis coupling proportional to x. If the offset has the same sign
// at both x extremes it is this, not coupling, and a signed cross-axis term
// would correct one side and double the other.
//
// Stages 4 and 5 fix the mechanical gate asymmetry (deterministic).
// Stage 6 addresses near-centre cross-axis leakage (non-deterministic;
// a deadzone is the only honest tool for a non-repeatable artifact).

#ifndef STICK_GEOMETRY_H
#define STICK_GEOMETRY_H

// Bump this whenever the public API changes, so a stale copy of this header
// fails with one clear message instead of a page of "has no member named".
//   1 = initial pipeline
//   2 = adds GateMap (circle <-> square) as stage 8
//   3 = adds cross-axis Y bias removal as stage 3
#define STICK_GEOMETRY_VERSION 3

#include <cstdint>
#include <cmath>

namespace sg {

static constexpr int   LUT_N     = 64;
static constexpr int   BIAS_N    = 12;   // bins over |x| in [0,1]
static constexpr float NEG_SCALE = 32768.0f;
static constexpr float POS_SCALE = 32767.0f;
static constexpr float TWO_PI    = 6.28318530718f;

// ---------------------------------------------------------------------------
// Measured geometry of one physical stick. Produced by StickCal.
// ---------------------------------------------------------------------------
struct Calibration
{
    // Resting centre, in normalised units. Usually tiny; subtract it anyway.
    float cx = 0.0f;
    float cy = 0.0f;

    // Per-direction reach at the gate, normalised. 1.0 means that direction
    // reaches electrical full scale. 0.94 means the gate stops it at 94%.
    // These are what a short gate on one side actually looks like in data.
    float neg_x = 1.0f;
    float pos_x = 1.0f;
    float neg_y = 1.0f;
    float pos_y = 1.0f;

    // Measured Y offset as a function of |x|, per side of X, sampled at bin
    // centres (i + 0.5) / BIAS_N. Zero is anchored at x = 0. These are the
    // values you MEASURED (e.g. -0.034), not the correction -- process()
    // subtracts them.
    bool  use_bias = false;
    float bias_neg[BIAS_N] = {};   // applies when x < 0, indexed by |x|
    float bias_pos[BIAS_N] = {};   // applies when x >= 0

    // Residual gate shape after the four gains: max radius per angle bin,
    // normalised so the largest bin is 1.0. Handles flats and octagonal
    // gates that four scalars cannot express. Set use_lut=false to skip.
    bool  use_lut = false;
    float rmax[LUT_N] = {};
};

// ---------------------------------------------------------------------------
// Circle <-> square domain conversion.
//
// ToSquare is the "rectangular algorithm": the circular gate is mapped onto a
// square output region, so a diagonal push reaches (1,1) instead of
// (0.707, 0.707). The scale factor is the square's boundary radius at the
// current angle:
//
//     k = 1 / max(|cos t|, |sin t|)  =  r / max(|x|, |y|)
//
// which is 1.000 on the axes and 1.414 on the diagonals.
//
// WORTH KNOWING BEFORE YOU A/B THIS: on the axes k is exactly 1, so a clean
// square map is an IDENTITY on pure-X and pure-Y input. It cannot manufacture
// a Y excursion from a zero-Y input, and near the axis it barely amplifies an
// existing one (at x=0.95, y=0.02 the factor is 1.0002). So if turning the
// rectangular algorithm on genuinely produces a cross-axis bulge, then BBW's
// implementation is NOT a clean radial square map -- and that is itself the
// finding. Comparing this stage against theirs is how you tell.
//
// ToCircle is the exact inverse (k = max(|x|,|y|) / r), for undoing a square
// map applied upstream so you can measure what is underneath it.
// ---------------------------------------------------------------------------
enum class GateMap { None, ToSquare, ToCircle };

// ---------------------------------------------------------------------------
// What you want done to the corrected signal.
// ---------------------------------------------------------------------------
struct Shape
{
    float dz_x        = 0.0f;   // axial deadzone, fraction of corrected range
    float dz_y        = 0.0f;
    bool  dz_rescale  = false;  // false = hard clip to 0 (keeps the input grid)
    bool  clamp_circle = true;  // clamp corrected radius to 1
    float out_scale   = 1.0f;   // radius that maps to full output (stick-edge
                                // equivalent). 0.96 == BBW "Stick edge -4"-ish.
    GateMap gate_map      = GateMap::None;
    float   gate_strength = 1.0f;   // 0 = identity, 1 = full mapping. Blend for A/B.
    int   quant_bits  = 0;      // 0 = off. 8 = 256 steps. Applied last.
};

// ---------------------------------------------------------------------------
inline float norm(int16_t v)
{
    return v < 0 ? (float)v / NEG_SCALE : (float)v / POS_SCALE;
}

inline int16_t denorm(float f)
{
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;
    float s = (f < 0.0f) ? f * NEG_SCALE : f * POS_SCALE;
    int32_t i = (int32_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
    if (i < -32768) i = -32768;
    if (i >  32767) i =  32767;
    return (int16_t)i;
}

// Integer bit-shift quantisation — same scheme as Range::quantize<N>.
inline int16_t quantize(int16_t v, int bits)
{
    if (bits <= 0 || bits >= 16) return v;
    const int shift = 16 - bits;
    return (int16_t)((v >> shift) << shift);
}

// Interpolate the bias profile at |x|. Anchored to 0 at the centre, flat
// beyond the last bin centre, linear in between.
inline float bias_at(const float* lut, float ax)
{
    if (ax <= 0.0f) return 0.0f;
    const float step = 1.0f / (float)BIAS_N;
    float t = ax / step - 0.5f;              // position in bin-centre units
    if (t <= 0.0f) return lut[0] * (ax / (0.5f * step));   // ramp from 0
    int i = (int)t;
    if (i >= BIAS_N - 1) return lut[BIAS_N - 1];
    float f = t - (float)i;
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

// Linear interpolation into the angular LUT, with wrap-around.
inline float lut_rmax(const Calibration& c, float ang)
{
    if (ang < 0.0f) ang += TWO_PI;
    float t  = ang * (float)LUT_N / TWO_PI;
    int   i0 = (int)t;
    float f  = t - (float)i0;
    i0 %= LUT_N;
    int i1 = (i0 + 1) % LUT_N;
    float a = c.rmax[i0], b = c.rmax[i1];
    if (a <= 0.0f) a = 1.0f;          // unpopulated bin: no correction
    if (b <= 0.0f) b = 1.0f;
    return a + (b - a) * f;
}

// Scaled or hard axial deadzone.
//   hard   : |v| <= dz -> 0, otherwise untouched. Preserves the input grid.
//   scaled : remaps [dz,1] -> [0,1]. Recovers range, multiplies the step
//            size by 1/(1-dz) — pair with quant_bits or you de-grid.
inline float axial_deadzone(float v, float dz, bool rescale)
{
    if (dz <= 0.0f) return v;
    float a = std::fabs(v);
    if (a <= dz) return 0.0f;
    if (!rescale) return v;
    float m = (a - dz) / (1.0f - dz);
    return v < 0.0f ? -m : m;
}

// ---------------------------------------------------------------------------
// The pipeline. Allocation-free, branch-light, safe to call at 1 kHz+.
// ---------------------------------------------------------------------------
inline void process(const Calibration& c, const Shape& s,
                    int16_t in_x, int16_t in_y,
                    int16_t& out_x, int16_t& out_y)
{
    // 1. normalise
    float x = norm(in_x);
    float y = norm(in_y);

    // 2. centre offset
    x -= c.cx;
    y -= c.cy;

    // 3. cross-axis Y bias removal. Subtracting a measured negative offset
    //    pushes Y back up; the per-side LUTs mean an offset that is negative
    //    on BOTH sides is handled correctly, which a signed coupling term
    //    would not be.
    if (c.use_bias)
        y -= bias_at(x < 0.0f ? c.bias_neg : c.bias_pos, std::fabs(x));

    // 4. per-direction gain — this is the gate-asymmetry fix.
    //    Dividing by a reach of 0.94 expands that direction back to full range.
    {
        float gx = (x < 0.0f) ? c.neg_x : c.pos_x;
        float gy = (y < 0.0f) ? c.neg_y : c.pos_y;
        if (gx > 0.01f) x /= gx;
        if (gy > 0.01f) y /= gy;
    }

    // 5. angular normalisation — residual shape (flats, octagonal corners)
    if (c.use_lut)
    {
        float r = std::sqrt(x * x + y * y);
        if (r > 1e-4f)
        {
            float rm = lut_rmax(c, std::atan2(y, x));
            if (rm > 0.01f)
            {
                float k = 1.0f / rm;
                x *= k;
                y *= k;
            }
        }
    }

    // 6. clamp to the unit circle
    if (s.clamp_circle)
    {
        float r = std::sqrt(x * x + y * y);
        if (r > 1.0f)
        {
            float k = 1.0f / r;
            x *= k;
            y *= k;
        }
    }

    // 7. axial deadzone
    x = axial_deadzone(x, s.dz_x, s.dz_rescale);
    y = axial_deadzone(y, s.dz_y, s.dz_rescale);

    // 8. output ceiling: radius out_scale maps to full output.
    //    Radial, not per-axis, so it does not distort direction.
    if (s.out_scale > 0.01f && s.out_scale < 0.999f)
    {
        float r = std::sqrt(x * x + y * y);
        if (r > 1e-4f)
        {
            float rn = r / s.out_scale;
            if (rn > 1.0f) rn = 1.0f;
            float k = rn / r;
            x *= k;
            y *= k;
        }
    }

    // 9. gate map: circle <-> square. Runs in the normalised circular domain,
    //    after every radial operation, so nothing downstream can undo it.
    if (s.gate_map != GateMap::None && s.gate_strength > 0.0f)
    {
        float ax = std::fabs(x), ay = std::fabs(y);
        float m  = ax > ay ? ax : ay;
        float r  = std::sqrt(x * x + y * y);
        if (m > 1e-4f && r > 1e-4f)
        {
            float k = (s.gate_map == GateMap::ToSquare) ? (r / m) : (m / r);
            if (s.gate_strength < 1.0f)
                k = 1.0f + (k - 1.0f) * s.gate_strength;
            x *= k;
            y *= k;
        }
    }

    // 10. quantise last. denorm() clamps each axis to [-1,1] independently,
    //    which is the square clamp the mapping needs -- do NOT add a radial
    //    clamp here or you undo stage 8.
    out_x = quantize(denorm(x), s.quant_bits);
    out_y = quantize(denorm(y), s.quant_bits);
}

// Identity calibration — use as the A/B control.
inline Calibration identity()
{
    Calibration c;
    for (int i = 0; i < LUT_N; ++i) c.rmax[i] = 1.0f;
    return c;
}

} // namespace sg

#endif // STICK_GEOMETRY_H
