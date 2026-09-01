/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/* ===========================================================================
 *  hid_app.c  -  BIGBIG WON Rainbow 2 Pro support (XINPUT mode)
 * ===========================================================================
 *
 *  PUT THE CONTROLLER IN XINPUT MODE FIRST (this is its DEFAULT):
 *
 *     Hold  B + HOME  for ~3 seconds until the HOME light turns GREEN.
 *
 *  In Xinput mode the Rainbow 2 Pro enumerates as a WIRED Xbox 360 controller.
 *  That is NOT a HID device - it uses a vendor-specific interface
 *  (class 0xFF, subclass 0x5D, protocol 0x01), so TinyUSB's HID host never
 *  sees it. We therefore register a small XInput HOST class driver
 *  (src/xinput_host.c) and consume its parsed reports here.
 *
 *  Plumbing:
 *    - usbh_app_driver_get_cb() below registers the XInput host class driver
 *      with TinyUSB (works since tinyusb PR #2222, present in pico-sdk 2.1.1).
 *    - tuh_xinput_mount_cb()  : controller attached -> start the report stream.
 *    - tuh_xinput_report_received_cb() : every input report -> fill gamepad(0).
 *    - hid_app_task()         : push rumble back to the controller.
 *
 *  XInput host driver: Ryzee119/tusb_xinput (MIT). See src/xinput_host.c.
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "bsp/board_api.h"
#include "tusb.h"
#include "host/usbh.h"
#include "xinput_host.h"
#include "tusb_gamepad.h"
<<<<<<< HEAD
#include "pad_config.h"
#include "axial_deadzone.h"
#include "stick_geometry_bridge.h"
#include "trigger_utils.h"
#include "macro_types.h"
#include "macro_engine.h"
#include "profile_store.h"
=======
#include "stick_correction.h"
>>>>>>> 4523dba (back now)

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static bool    xinput_mounted = false;
static uint8_t xinput_dev_addr = 0;
static uint8_t xinput_instance = 0;


//--------------------------------------------------------------------+
// Phase 4 helpers: macro engine bridge + on-pad profile switching
//--------------------------------------------------------------------+

// Translate the tusb_gamepad bitfield into the ReflexX button mask the macro
// engine speaks. Kept as an explicit mapping rather than a memcpy/union because
// the two layouts genuinely differ: tusb_gamepad orders its bits by controller
// ergonomics, ReflexX inherits XInput's wButtons ordering, and `misc` has no
// XInput home at all (see GP_MISC in macro_types.h).
static uint32_t buttons_to_mask(const GamepadButtons *b)
{
    uint32_t m = 0;
    if (b->up)    m |= GP_DPAD_UP;
    if (b->down)  m |= GP_DPAD_DOWN;
    if (b->left)  m |= GP_DPAD_LEFT;
    if (b->right) m |= GP_DPAD_RIGHT;
    if (b->a)     m |= GP_A;
    if (b->b)     m |= GP_B;
    if (b->x)     m |= GP_X;
    if (b->y)     m |= GP_Y;
    if (b->l3)    m |= GP_LEFT_THUMB;
    if (b->r3)    m |= GP_RIGHT_THUMB;
    if (b->back)  m |= GP_BACK;
    if (b->start) m |= GP_START;
    if (b->lb)    m |= GP_LEFT_SHOULDER;
    if (b->rb)    m |= GP_RIGHT_SHOULDER;
    if (b->sys)   m |= GP_GUIDE;
    if (b->misc)  m |= GP_MISC;
    return m;
}

static void mask_to_buttons(uint32_t m, GamepadButtons *b)
{
    b->up    = (m & GP_DPAD_UP)        ? 1 : 0;
    b->down  = (m & GP_DPAD_DOWN)      ? 1 : 0;
    b->left  = (m & GP_DPAD_LEFT)      ? 1 : 0;
    b->right = (m & GP_DPAD_RIGHT)     ? 1 : 0;
    b->a     = (m & GP_A)              ? 1 : 0;
    b->b     = (m & GP_B)              ? 1 : 0;
    b->x     = (m & GP_X)              ? 1 : 0;
    b->y     = (m & GP_Y)              ? 1 : 0;
    b->l3    = (m & GP_LEFT_THUMB)     ? 1 : 0;
    b->r3    = (m & GP_RIGHT_THUMB)    ? 1 : 0;
    b->back  = (m & GP_BACK)           ? 1 : 0;
    b->start = (m & GP_START)          ? 1 : 0;
    b->lb    = (m & GP_LEFT_SHOULDER)  ? 1 : 0;
    b->rb    = (m & GP_RIGHT_SHOULDER) ? 1 : 0;
    b->sys   = (m & GP_GUIDE)          ? 1 : 0;
    b->misc  = (m & GP_MISC)           ? 1 : 0;
}

// On-pad profile switching: hold Back and tap DPad-Up / DPad-Down to cycle the
// active slot. RAM-only, so it resets to whatever slot the flash image marks
// active on the next power-cycle - persisting every switch would mean a flash
// erase from inside the XInput report path, which is exactly the thing
// profile_store.h forbids (XIP goes away and core 1 faults).
//
// The combo's buttons are consumed (stripped from the outgoing report) while it
// is engaged, so the game does not also see a menu-open + dpad press. There is
// deliberately no process-based auto-switching: this hardware has no way to know
// what game is running.
static void apply_profile_combo(GamepadButtons *btns)
{
    static bool was_up = false, was_down = false;

    if (!btns->back) { was_up = false; was_down = false; return; }

    bool up   = btns->up;
    bool down = btns->down;
    uint32_t slots = PROFILE_SLOTS;
    uint32_t cur = profile_store_active_index();

    if (up && !was_up)        profile_store_set_active((cur + 1) % slots);
    else if (down && !was_down) profile_store_set_active((cur + slots - 1) % slots);

    was_up = up;
    was_down = down;

    // Consume: Back+DPad is a live combo, not player input.
    if (up || down) { btns->up = 0; btns->down = 0; btns->back = 0; }
}

//--------------------------------------------------------------------+
// Register the XInput host class driver with TinyUSB.
// TinyUSB calls this to discover application-provided custom class drivers.
//--------------------------------------------------------------------+

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count)
{
    *driver_count = 1;
    return &usbh_xinput_driver;
}

//--------------------------------------------------------------------+
// Application task (runs on core1 inside the usb-host loop)
//--------------------------------------------------------------------+

void hid_app_task(void)
{
    if (!xinput_mounted) return;

    const uint32_t interval_ms = 100;
    static uint32_t start_ms = 0;

    uint32_t now = board_millis();
    if (now - start_ms >= interval_ms)
    {
        start_ms = now;
        // Re-arm the IN endpoint in case a previous receive_report failed
        tuh_xinput_receive_report(xinput_dev_addr, xinput_instance);
    }
}

//--------------------------------------------------------------------+
// XInput report -> Gamepad
//--------------------------------------------------------------------+

static void process_xinput(const xinput_gamepad_t* p)
{
    Gamepad* gp = gamepad(0);

    // Build state in locals first, then write to the shared gamepad in one
    // shot so core 0 never sees a half-zeroed struct.
    GamepadButtons   btns = {0};
    GamepadTriggers  trig = {0};
    GamepadJoysticks joy  = {0};

    // D-pad
    if (p->wButtons & XINPUT_GAMEPAD_DPAD_UP)    btns.up    = 1;
    if (p->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  btns.down  = 1;
    if (p->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  btns.left  = 1;
    if (p->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) btns.right = 1;

    // Face buttons
    if (p->wButtons & XINPUT_GAMEPAD_A) btns.a = 1;
    if (p->wButtons & XINPUT_GAMEPAD_B) btns.b = 1;
    if (p->wButtons & XINPUT_GAMEPAD_X) btns.x = 1;
    if (p->wButtons & XINPUT_GAMEPAD_Y) btns.y = 1;

    // Bumpers / stick clicks
    if (p->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns.lb = 1;
    if (p->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns.rb = 1;
    if (p->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns.l3 = 1;
    if (p->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns.r3 = 1;

    // Menu buttons
    if (p->wButtons & XINPUT_GAMEPAD_BACK)  btns.back  = 1;
    if (p->wButtons & XINPUT_GAMEPAD_START) btns.start = 1;
    if (p->wButtons & XINPUT_GAMEPAD_GUIDE) btns.sys   = 1;
    if (p->wButtons & XINPUT_GAMEPAD_SHARE) btns.misc  = 1;

    // Thumbsticks (already int16, same convention)
    joy.lx = p->sThumbLX;
    joy.ly = p->sThumbLY;
    joy.rx = p->sThumbRX;
    joy.ry = p->sThumbRY;

<<<<<<< HEAD
    // --- On-pad profile switching (before anything reads the config) ---
    // Runs first so a slot change takes effect on this very report rather than
    // the next one, and so the combo buttons are stripped before the macro engine
    // could interpret Back/DPad as somebody's activation button.
    apply_profile_combo(&btns);

    // --- Stick/trigger processing pipeline (Phase 3) ---
    // Filter knobs now come from the active on-flash profile rather than the
    // compile-time constant. profile_store_init() guarantees a valid working copy
    // (falling back to g_pad_config on blank/corrupt flash), so this pointer is
    // always good and the behaviour with an unconfigured board is byte-identical
    // to the previous firmware.
    const pad_config_t* cfg = &profile_store_active()->filters;

    if (cfg->left_stick_axial_deadzone) {
        uint16_t dz = (uint16_t)cfg->left_stick_axial_deadzone * 256;
        joy.lx = axial_deadzone_s16(joy.lx, dz);
        joy.ly = axial_deadzone_s16(joy.ly, dz);
    }

    if (cfg->right_stick_axial_deadzone) {
        uint16_t dz = (uint16_t)cfg->right_stick_axial_deadzone * 256;
        joy.rx = axial_deadzone_s16(joy.rx, dz);
        joy.ry = axial_deadzone_s16(joy.ry, dz);
    }

    // Right stick: StickGeometry.h pipeline (compiled-in identity calibration
    // -- see stick_geometry_bridge.cpp) plus the same soft corner cap as
    // before (right_stick_corner_cap_pct). Always on; degenerates to
    // passthrough + corner cap until the calibration is measured and filled
    // in, matching the previous uncap_radius=true default byte-for-byte.
    stick_geometry_process_right(&joy.rx, &joy.ry, cfg->right_stick_corner_cap_pct);

    // Triggers: 8-bit in, 8-bit out, no intermediate quantization.
    trig.l = cfg->trigger_l_instant
        ? apply_trigger_instant(p->bLeftTrigger,  cfg->trigger_l_threshold)
        : apply_trigger_limit  (p->bLeftTrigger,  cfg->trigger_l_max);
    trig.r = cfg->trigger_r_instant
        ? apply_trigger_instant(p->bRightTrigger, cfg->trigger_r_threshold)
        : apply_trigger_limit  (p->bRightTrigger, cfg->trigger_r_max);

    // --- Macro engine (Phase 4) ---
    // Deliberately placed AFTER the filter pipeline and BEFORE the commit, for
    // the same reason ReflexX runs CompositeInputFilter before MacroProcessor:
    // macros reason about clean, deadzone-corrected, shaped input (e.g. CrowBar
    // asks "is the player pulling down past 5% of range?", AutoSprint asks "is
    // forward past 60%?"). Feeding them raw sensor values with a jittery centre
    // would make every threshold behave differently per controller. Writing after
    // the filters also means a macro's synthesized deflection is not re-scaled by
    // a deadzone rescale it was never meant to pass through.
    //
    // Zero-macro profiles cost one null/count check inside macro_engine_process(),
    // so an unconfigured board's path is unchanged.
    {
        macro_gamepad_state_t ms;
        ms.buttons = buttons_to_mask(&btns);
        ms.lx = joy.lx; ms.ly = joy.ly;
        ms.rx = joy.rx; ms.ry = joy.ry;
        ms.lt = trig.l; ms.rt = trig.r;
        ms._pad[0] = ms._pad[1] = 0;

        macro_engine_process(&ms);

        mask_to_buttons(ms.buttons, &btns);
        joy.lx = ms.lx; joy.ly = ms.ly;
        joy.rx = ms.rx; joy.ry = ms.ry;
        trig.l = ms.lt; trig.r = ms.rt;
    }
=======
    // Anti-snapback correction (left stick untouched).
    correct_right_stick(&joy.rx, &joy.ry);
>>>>>>> 4523dba (back now)

    // Commit to shared gamepad — no zero window visible to core 0
    gp->buttons   = btns;
    gp->triggers  = trig;
    gp->joysticks = joy;

    __dmb();
}

//--------------------------------------------------------------------+
// XInput host callbacks (implemented from xinput_host.h)
//--------------------------------------------------------------------+

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t* xinput_itf)
{
    printf("XInput device mounted: addr = %d, instance = %d, type = %d\r\n",
           dev_addr, instance, xinput_itf->type);

    // Xbox 360 Wireless receivers report a mount before a controller is
    // actually connected; just keep polling until a connection arrives.
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false)
    {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }

    if (!xinput_mounted)
    {
        xinput_dev_addr = dev_addr;
        xinput_instance = instance;
        xinput_mounted = true;
    }

    // Light player-1 LED, clear rumble, then start the input stream.
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("XInput device unmounted: addr = %d, instance = %d\r\n", dev_addr, instance);

    if (xinput_mounted && xinput_dev_addr == dev_addr && xinput_instance == instance)
    {
        xinput_mounted = false;
    }
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                   xinputh_interface_t const* xid_itf, uint16_t len)
{
    (void)len;

    if (xid_itf->last_xfer_result == XFER_RESULT_SUCCESS &&
        xid_itf->connected && xid_itf->new_pad_data)
    {
        process_xinput(&xid_itf->pad);
    }

    // keep the IN pipe armed
    tuh_xinput_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------+
// HID host callbacks (required by TinyUSB's HID host class, unused here)
//--------------------------------------------------------------------+

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
    (void)dev_addr; (void)instance; (void)desc_report; (void)desc_len;
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr; (void)instance;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void)dev_addr; (void)instance; (void)report; (void)len;
}