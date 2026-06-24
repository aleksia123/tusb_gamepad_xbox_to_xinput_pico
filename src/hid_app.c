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

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static bool    xinput_mounted = false;
static uint8_t xinput_dev_addr = 0;
static uint8_t xinput_instance = 0;
static uint8_t motor_left  = 0;
static uint8_t motor_right = 0;

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
        // Push the host's requested rumble to the controller (non-blocking).
        tuh_xinput_set_rumble(xinput_dev_addr, xinput_instance, motor_left, motor_right, false);
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

    // Triggers (already 0..255)
    trig.l = p->bLeftTrigger;
    trig.r = p->bRightTrigger;

    // Thumbsticks (already int16, same convention)
    joy.lx = p->sThumbLX;
    joy.ly = p->sThumbLY;
    joy.rx = p->sThumbRX;
    joy.ry = p->sThumbRY;

    // Commit to shared gamepad — no zero window visible to core 0
    gp->buttons   = btns;
    gp->triggers  = trig;
    gp->joysticks = joy;

    __dmb();

    // Pass the host's requested rumble back out (sent in hid_app_task()).
    motor_left  = gp->rumble.l;
    motor_right = gp->rumble.r;
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
        motor_left  = 0;
        motor_right = 0;
        xinput_mounted = true;
    }

    // Light player-1 LED, clear rumble, then start the input stream.
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
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
