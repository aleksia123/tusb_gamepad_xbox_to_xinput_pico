// stick_geometry_bridge.h - C-callable entry point into StickGeometry.h
// (C++17 header-only) for the right stick.
//
// Replaces stick_radial.h. That header had two independent stages: a
// runtime-learned gain + hard-circle correction (Stage 1, OFF by default via
// uncap_radius=true) and a soft corner cap (Stage 2, ON by default at 120%).
// This bridge runs StickGeometry.h's pipeline in place of Stage 1 -- with an
// IDENTITY calibration (see stick_geometry_bridge.cpp) it is a no-op, exactly
// matching Stage-1-disabled behaviour -- then re-applies the same soft
// corner-cap math as Stage 2, so the shipped default feel is unchanged.
//
// Calibration is compiled-in, same as pad_config.h (see that header's
// comment: no live config channel exists on this board). To correct actual
// measured stick geometry, edit the sg::Calibration values in the .cpp.
//
// Left-stick deadzone (axial_deadzone.h) is untouched and still runs first
// in hid_app.c; this bridge does not re-apply deadzone, it only replaces the
// old right-stick radial stage.
#ifndef STICK_GEOMETRY_BRIDGE_H
#define STICK_GEOMETRY_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// corner_cap_pct: 0 = off, 100 = hard unit circle, 101..255 = soft cap at
// that percent of full scale. Same semantics as pad_config_t's
// right_stick_corner_cap_pct (still the caller's compiled-in knob).
void stick_geometry_process_right(int16_t* rx, int16_t* ry, uint8_t corner_cap_pct);

#ifdef __cplusplus
}
#endif

#endif // STICK_GEOMETRY_BRIDGE_H
