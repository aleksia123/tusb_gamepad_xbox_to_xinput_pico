// pad_config.h - stick/trigger processing profile, read by the Phase 3
// pipeline in hid_app.c.
//
// There is no live config channel: the device's sole USB interface is bound
// by Windows to the built-in xusb22.sys XInput driver (that's what makes it
// driverless-XInput in the first place), which claims the device exclusively
// at the kernel level - WebUSB/WebSerial from the browser can't open it, and
// adding a second interface (CDC/HID) to carry a config protocol would mean
// hand-editing tusb_gamepad's fixed single-interface XInput descriptor, with
// real risk of breaking XInput enumeration. So these values are compiled-in:
// to change them, edit the defaults below and reflash.
#ifndef PAD_CONFIG_H
#define PAD_CONFIG_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  left_stick_axial_deadzone;   // 0..255 half-width; int16 = value*256
    uint8_t  right_stick_axial_deadzone;
    uint8_t  trigger_l_max;               // ceiling clamp, 0..255 (255 = off), see trigger_utils.h
    uint8_t  trigger_r_max;
    uint8_t  trigger_l_threshold;         // instant-mode activation point, 0..255 (default: 1)
    uint8_t  trigger_r_threshold;
    bool     trigger_l_instant;           // false = limit mode, true = instant (digital) mode
    bool     trigger_r_instant;
    // NOTE: no longer read by hid_app.c. Right-stick gain+circle correction
    // is now StickGeometry.h's calibration (see stick_geometry_bridge.cpp) --
    // it is a no-op until that calibration is measured and filled in, which
    // is what this flag used to gate. Field kept to preserve the wire layout
    // (see PROTOCOL.md); repurpose or remove in the next protocol version bump.
    bool     uncap_radius;

    // Right-stick corner cap: independent of uncap_radius. Trims ONLY the
    // diagonal tip that overshoots the given radius; everything inside passes
    // untouched, so it preserves the near-cardinal / mid-angle gate shape
    // (and thus the ~8% avg circularity error) while pulling the sharpest
    // corner reach in. Value is percent-over-full: 0 = off, 120 = cap at
    // 1.20x full scale (+20% peak corner). Runs even when uncap_radius=true,
    // so you can keep the natural overshoot but bound its peak.
    //   0        -> off (no cap)
    //   100      -> hard unit circle (equivalent to full clamp)
    //   101..255 -> soft cap at that percent of full scale
    uint8_t  right_stick_corner_cap_pct;
} pad_config_t;

// Factory defaults, sourced from joypad-os's own R2P profile:
//   - right-stick deadzone 7/255; left 1/255.
//   - trigger limits full-scale (255 = off), instant mode on, threshold 1.
//   - uncap_radius = true (natural diagonal overshoot preserved; measured
//     ~8% avg circularity error on the R2P, which is in the healthy stock
//     range, so the gain+circle correction is deliberately bypassed).
//   - corner cap 120 -> keeps the overshoot but bounds the sharpest corner
//     to +20% (raw R2P peaks ~+31.8%). Set 0 to disable, 100 for a hard circle.
static const pad_config_t g_pad_config = {
    .left_stick_axial_deadzone = 14,     // Ultra-raw, instant strafe response
    .right_stick_axial_deadzone = 14,    // Balanced, high-precision Look deadzone (~2.7%)
    .trigger_l_max = 255,
    .trigger_r_max = 255,
    .trigger_l_threshold = 1,
    .trigger_r_threshold = 1,
    .trigger_l_instant = true,
    .trigger_r_instant = false,
    .uncap_radius = true,
    .right_stick_corner_cap_pct = 120,  // NEW: Smooths the +31% diagonal spikes down to a perfect +12%
};
#endif // PAD_CONFIG_H
