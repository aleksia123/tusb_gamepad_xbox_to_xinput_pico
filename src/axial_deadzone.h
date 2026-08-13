// axial_deadzone.h - per-axis (axial) deadzone in the tusb_gamepad int16 stick space
// (center 0, half-travel 32768). Scaled survivor rescale, so no jump at the edge.
//
// Purpose: clip the small spurious Y on the R2P right stick (the "bulge") at the
// source. Axial = each axis judged on its OWN magnitude - that is what reaches a
// small-Y-riding-on-large-X bulge; a RADIAL deadzone is dominated by the large RX
// and never touches RY at deflection, so it does NOT fix the bulge. Keep this
// independent of the radial/uncap-radius stage (stick_radial.h) - run it first,
// as a per-axis pre-clean.
//
// dz_units is a half-width in the same int16 scale as the axis (0..32768).
#ifndef AXIAL_DEADZONE_H
#define AXIAL_DEADZONE_H
#include <stdint.h>

static inline int16_t axial_deadzone_s16(int16_t axis, uint16_t dz_units) {
    if (dz_units == 0) return axis;                        // gate: no-op when disabled
    int32_t mag = (axis < 0) ? -(int32_t)axis : axis;       // 0..32768
    if (mag <= dz_units) return 0;                          // inside deadzone -> center
    int32_t span   = 32768 - dz_units;                      // remaining travel
    int32_t scaled = ((mag - dz_units) * 32767) / span;     // rescale survivor to full
    return (axis < 0) ? (int16_t)(-scaled) : (int16_t)scaled;
}

#endif // AXIAL_DEADZONE_H
