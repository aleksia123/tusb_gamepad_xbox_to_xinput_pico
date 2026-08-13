// config_mode.h - boot-time configuration mode over USB CDC.
//
// ═══ WHY A SEPARATE BOOT MODE ════════════════════════════════════════════════
// pad_config.h documents the blocker: while this board is acting as an XInput
// pad, Windows binds its single interface to the in-box xusb22.sys driver, which
// claims the device exclusively at kernel level. No WebUSB/WebSerial page can
// open it, and grafting a second (CDC) interface onto tusb_gamepad's fixed XInput
// descriptor means hand-editing that descriptor with a real chance of breaking
// XInput enumeration - the one thing that makes this dongle driverless.
//
// So configuration is not live-linked into the pad path at all, and switching
// between the two personalities always goes through a reset (never a live
// descriptor swap) - see boot_request.h.
//
// ── CONFIG MODE IS THE DEFAULT ON EVERY POWER-ON ─────────────────────────────
// No jumper, no combo: plug the board in (or tap RUN/reset) and it always
// enumerates first as a plain USB serial device ("TinyUSB Device") with no
// XInput interface and no USB host stack. Open tools/configurator/index.html
// in Chrome or Edge and pick that port.
//
// If nothing talks to it within CONFIG_GRACE_MS (config_mode.c), or if the
// host sends REBOOT, it warm-reboots straight into normal XInput operation.
// That warm reboot is remembered (boot_request.h) so a RUN-button reset
// during actual play does NOT sit through the grace window again - only an
// honest-to-goodness power cycle (unplug/replug) ever does, and even then
// it's bounded to CONFIG_GRACE_MS, not an open-ended wait.
//
// GP29 changes what happens during that window rather than whether it
// happens: ground it before powering on to suspend the grace timer entirely
// (stay in config mode indefinitely, however long the edit session takes),
// instead of racing a fixed clock. Useful for a slow first-time setup; leave
// it floating for the normal "plug in, edit within a few seconds, or it just
// becomes a controller" flow.
//
// GP29 was chosen because it is not spoken for and, unlike GP15, is actually
// broken out to a header pin on the Waveshare RP2350-USB-A (which exposes
// only GP0-10 and GP26-29 - GP15 lives on the die but isn't wired to either
// header on this board). GP12/GP13 carry PIO-USB D+/D- (see PIO_USB_DP_PIN in
// main.c) and are off-header entirely; nothing else in this project or in
// board_init() claims a GPIO. Active-low with the RP2350's internal pull-up,
// so an unconnected pin reads high and the timed grace window is the default
// with no jumper and no external parts.
#ifndef CONFIG_MODE_H
#define CONFIG_MODE_H

#include <stdbool.h>

// GPIO sampled once, very early in config_mode_run(). Active low.
#define CONFIG_MODE_PIN 29

// True unless this boot is a warm reset left behind by
// boot_request_go_xinput() (see boot_request.h) - i.e. true on every real
// power-on, false for the one boot immediately after the grace window
// elapsed or the host sent REBOOT. Safe to call before board_init().
bool config_mode_requested(void);

// Brings up the CDC device and runs the request/response loop. Returns only
// via a warm reboot (grace-window timeout or an explicit REBOOT command) -
// config mode is a distinct operating mode, not a phase, so there is no live
// path back into pad operation. That is deliberate: it removes any
// possibility of a flash write racing a live XInput stream, and avoids
// hot-swapping tinyusb's device descriptor mid-session.
void config_mode_run(void);

#endif // CONFIG_MODE_H
