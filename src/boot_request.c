// boot_request.c - see boot_request.h for why this reads a reset-cause register
// instead of trusting a scratch flag on its own.
#include "boot_request.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/structs/powman.h"

// Arbitrary; only has to be a value that a cleared register cannot hold.
#define BOOT_REQUEST_SKIP_CONFIG_MAGIC 0x52584331u // "RXC1"

// POWMAN SCRATCH0. Not password protected (datasheet 6.4: everything at offset
// > 0xac takes plain 32-bit writes), not touched by the bootrom (that uses
// POWMAN BOOT0-3 and watchdog SCRATCH4-7), and not used anywhere else here.
#define BOOT_REQUEST_FLAG (powman_hw->scratch[0])

// Delay before the actual chip reset. Long enough for config_mode.c's REBOOT
// handler to have flushed its response, short enough not to read as a hang.
#define REBOOT_DELAY_MS 150u

// Cause bits are read-only and live in the always-on domain, so this is valid
// from the first instruction of main() onwards.
static uint32_t reset_cause(void)
{
    return powman_hw->chip_reset;
}

bool boot_request_was_cold_boot(void)
{
    return (reset_cause() & (POWMAN_CHIP_RESET_HAD_POR_BITS |
                             POWMAN_CHIP_RESET_HAD_BOR_BITS)) != 0u;
}

bool boot_request_skip_config(void)
{
    const uint32_t cause = reset_cause();

    // Consume the request unconditionally: whatever we decide below, the NEXT
    // reset must decide for itself. A firmware crash that reboots the chip then
    // falls back to config mode rather than looping invisibly as a pad.
    const bool armed = (BOOT_REQUEST_FLAG == BOOT_REQUEST_SKIP_CONFIG_MAGIC);
    BOOT_REQUEST_FLAG = 0u;

    // A real power-on or brown-out clears POWMAN, so it clears the flag too -
    // `armed` and a genuine cold boot are mutually exclusive by construction.
    // Checking this first means a stale HAD_RUN_LOW from before the power cut
    // (which cannot happen, but costs nothing to rule out) can never steal the
    // one boot that is supposed to land in config mode.
    if (!armed && boot_request_was_cold_boot()) {
        return false; // → config mode
    }

    // Our own warm reboot out of config mode. CHIP_RESET is stale here (a
    // watchdog PSM reset does not reset POWMAN), which is exactly why this
    // check exists and why it has to come before any cause-bit test.
    if (armed) {
        return true;
    }

    // RUN pin held low - the reset button, or a RUN-to-GND tap. POWMAN was hard
    // reset, so this bit is fresh, and it stays set through any later watchdog
    // reboot until the next real power cycle. That is the desired semantic:
    // once you have pressed reset, the board keeps coming up as a controller
    // until it is actually unplugged.
    if (cause & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) {
        return true;
    }

    // Debugger reset, rescue reset, glitch detector, a picotool reload, or a
    // watchdog reboot we did not ask for. All of these mean something other
    // than "the user is playing", so config mode is the useful default - and
    // after a reflash it puts the configurator one click away.
    return false;
}

void boot_request_go_xinput(void)
{
    // Must be armed before the reset is requested, not after: watchdog_reboot()
    // can fire as soon as it is called if the delay rounds to zero ticks.
    BOOT_REQUEST_FLAG = BOOT_REQUEST_SKIP_CONFIG_MAGIC;

    // watchdog_reboot() resets the PSM only; POWMAN_WDSEL is left at its reset
    // value of 0, so POWMAN - and with it SCRATCH0 - is untouched. Table 528:
    // "WATCHDOG RESET PSM" resets neither POWMAN nor AON Scratch. If anything
    // in this project ever writes POWMAN_WDSEL, this flag dies with it.
    watchdog_reboot(0, 0, REBOOT_DELAY_MS);
    while (true) { tight_loop_contents(); }
}