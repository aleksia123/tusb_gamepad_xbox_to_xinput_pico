// boot_request.h - decides, at every boot, whether this board comes up in
// config mode (see config_mode.h) or straight into XInput pad operation.
//
// THE RULE
//   Real power cycle (unplug/replug, brown-out) ....... config mode
//   RUN pin / reset button ........................... straight to XInput
//   Our own warm reboot out of config mode ........... straight to XInput
//   Anything else (debugger, crash, picotool) ........ config mode
//
// Only an honest power cycle ever pays the CONFIG_GRACE_MS window. Tapping
// RUN during play comes back up as a controller immediately.
//
// ═══ WHY THIS IS NOT A SINGLE SCRATCH FLAG ═══════════════════════════════════
// The obvious implementation - stash a magic word somewhere that survives a
// reset but not a power cycle - cannot work on RP2350, because a RUN-pin reset
// wipes every candidate location:
//
//   * Watchdog SCRATCH0-7: "The scratch registers reset when [...] a rst_n_run
//     event occurs, triggered by toggling the RUN pin or cycling the digital
//     core supply" (datasheet 12.9.5). So a RUN reset is indistinguishable from
//     a power-on by watchdog scratch alone - which is exactly the bug this file
//     used to have.
//   * POWMAN SCRATCH0-7 / BOOT0-3 (always-on domain): datasheet Table 528 lists
//     "EXTERNAL RESET (RUN)" as resetting AON Scratch, same as POR and BOR.
//   * SRAM: the bootrom re-initialises RAM before entering the image, so an
//     .uninitialized_data marker is not reliable either.
//
// What DOES survive is the reason itself. POWMAN_CHIP_RESET (offset 0x2c, in
// the always-on domain) records the cause of the last chip-level reset in
// read-only bits: HAD_POR, HAD_BOR, HAD_RUN_LOW, HAD_DP_RESET_REQ, ... So we
// ask the hardware "what reset you?" instead of trying to leave ourselves a
// note that would be shredded by the very event we want to detect.
//
// ── THE ONE CASE CHIP_RESET CANNOT ANSWER ────────────────────────────────────
// A watchdog reset that only resets the PSM (which is what watchdog_reboot()
// does - POWMAN_WDSEL is left at 0) does NOT reset POWMAN, so CHIP_RESET keeps
// whatever it said before: after our warm reboot out of config mode it still
// reads HAD_POR from the original power-on. Reset cause alone would therefore
// bounce straight back into config mode, forever.
//
// That single case is what the magic word is still for, and POWMAN SCRATCH0 is
// the correct home for it precisely because of the property that disqualified
// it above: it is cleared by POR, BOR and RUN, and preserved across exactly the
// watchdog reset we perform ourselves. So "scratch is armed" can only ever mean
// "we asked for this reboot", and never "the board was just plugged in".
//
// The two signals together are complete, and they cross-check: a genuine
// power-on always shows HAD_POR/HAD_BOR *and* an empty scratch.
#ifndef BOOT_REQUEST_H
#define BOOT_REQUEST_H

#include <stdbool.h>

// Call once, very early in main() - before any USB init, before board_init().
// True means "skip config mode, come up as an XInput pad". Consumes the warm-
// reboot request either way, so a crash loop can never wedge the pad out of
// config mode: the next reset that is not a RUN press lands back in it.
bool boot_request_skip_config(void);

// Arms the request and warm-reboots the chip almost immediately. Never returns.
// Safe to call from either core. Used by config_mode.c when the grace window
// elapses uncontacted, or when the host sends REBOOT.
void boot_request_go_xinput(void);

// True if this boot came from a real power-on or brown-out, i.e. the board was
// physically plugged in. Exposed for diagnostics (HELLO could report it) and
// because it is the only condition under which the grace window is paid.
bool boot_request_was_cold_boot(void);

#endif // BOOT_REQUEST_H