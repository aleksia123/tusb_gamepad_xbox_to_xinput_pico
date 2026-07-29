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
// So configuration is not live-linked into the pad path at all. Instead: ground
// one GPIO at boot and the firmware comes up as a plain USB serial device with no
// XInput interface and no USB host stack, purely to read and write the on-flash
// profile store. Normal boot (pin floating) never touches any of this - same
// descriptor, same single-interface XInput device, same latency, and every line
// of config-mode code is behind one branch in main() that runs once.
//
// The upside of routing it through tusb_gamepad's existing INPUT_MODE_USBSERIAL
// is that the CDC descriptor set already exists and is already known-good; we did
// not have to author or modify a descriptor at all.
//
// ── ENTERING CONFIG MODE ─────────────────────────────────────────────────────
//   1. Short GP15 to any GND pin.
//   2. Power-cycle the board (or tap RUN/reset).
//   3. It enumerates as a USB serial port ("TinyUSB Device"). Open
//      tools/configurator/index.html in Chrome or Edge and pick that port.
//   4. Remove the jumper and power-cycle to return to normal pad operation.
//
// GP15 was chosen because it is not spoken for: GP12/GP13 carry PIO-USB D+/D-
// (see PIO_USB_DP_PIN in main.c), and nothing else in this project or in
// board_init() claims a GPIO. Active-low with the RP2350's internal pull-up, so
// an unconnected pin reads high and normal boot is the default with no jumper and
// no external parts.
#ifndef CONFIG_MODE_H
#define CONFIG_MODE_H

#include <stdbool.h>

// GPIO sampled once, very early in main(). Active low.
#define CONFIG_MODE_PIN 15

// Configures the pin with a pull-up, lets it settle, and samples it. Safe to
// call before board_init(). Leaves the pull-up enabled - harmless, and it means
// a jumper inserted later cannot float.
bool config_mode_requested(void);

// Brings up the CDC device and runs the request/response loop forever. Never
// returns: config mode is a distinct operating mode, not a phase, so there is no
// path from here back into pad operation without a reset. That is deliberate -
// it removes any possibility of a flash write racing a live XInput stream.
void config_mode_run(void);

#endif // CONFIG_MODE_H
