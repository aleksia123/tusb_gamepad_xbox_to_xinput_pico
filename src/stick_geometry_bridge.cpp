// stick_geometry_bridge.cpp - see stick_geometry_bridge.h for why this exists.
#include "stick_geometry_bridge.h"
#include "StickGeometry.h"
#include <cmath>

namespace {

// Compiled-in right-stick calibration. Identity until someone measures the
// actual gate geometry (StickCal tooling referenced in StickGeometry.h's
// comments does not exist in this repo yet) -- with identity values, stages
// 2-6 of sg::process() are all no-ops, so this is byte-equivalent to the old
// Stage-1-disabled (uncap_radius=true) passthrough. Fill these in once a
// stick has been measured; no rebuild-time flag is needed to turn correction
// on, it activates the moment neg_x/pos_x/etc. stop being 1.0.
const sg::Calibration& right_stick_calibration()
{
    static const sg::Calibration cal = sg::identity();
    return cal;
}

// StickGeometry.h has no "soft cap only the tip beyond radius R" stage
// (its clamp_circle/out_scale stages both act on the whole range), so the
// old stick_radial.h Stage 2 math is kept verbatim here rather than forced
// through a stage it doesn't fit.
void soft_corner_cap(float& x, float& y, uint8_t corner_cap_pct)
{
    if (corner_cap_pct == 0) return;
    const float cap = 32767.0f * ((float)corner_cap_pct / 100.0f);
    const float mag = std::sqrt(x * x + y * y);
    if (mag > cap) {
        const float scale = cap / mag;
        x *= scale;
        y *= scale;
    }
}

} // namespace

extern "C" void stick_geometry_process_right(int16_t* rx, int16_t* ry, uint8_t corner_cap_pct)
{
    // Shape: no deadzone here (axial_deadzone.h already ran on this axis in
    // hid_app.c), no circle clamp / output ceiling / gate map -- the corner
    // cap below is the only shaping stage, matching the previous default.
    static const sg::Shape shape = [] {
        sg::Shape s;
        s.dz_x = s.dz_y = 0.0f;
        s.clamp_circle = false;
        s.out_scale = 1.0f;   // >= 0.999 -> stage is skipped
        s.gate_map = sg::GateMap::None;
        s.quant_bits = 0;
        return s;
    }();

    int16_t ox, oy;
    sg::process(right_stick_calibration(), shape, *rx, *ry, ox, oy);

    float fx = (float)ox;
    float fy = (float)oy;
    soft_corner_cap(fx, fy, corner_cap_pct);

    int32_t ix = (int32_t)(fx + (fx >= 0.0f ? 0.5f : -0.5f));
    int32_t iy = (int32_t)(fy + (fy >= 0.0f ? 0.5f : -0.5f));
    if (ix < -32768) ix = -32768;
    if (ix >  32767) ix =  32767;
    if (iy < -32768) iy = -32768;
    if (iy >  32767) iy =  32767;

    *rx = (int16_t)ix;
    *ry = (int16_t)iy;
}
