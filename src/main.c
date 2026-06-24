#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "host/usbh.h"
#include "bsp/board_api.h"
#include "pio_usb.h"
#include "tusb_gamepad.h"
#include "hardware/timer.h"

// ------------------------------------------------------------------ //
//  Board selection
//  Values don't matter as long as they're unique.
// ------------------------------------------------------------------ //
#define PI_PICO          1
#define ADAFRUIT_FEATHER 2
#define RP2350_USB_A     3   // Waveshare RP2350-USB-A

// >>> Choose your board here <<<
#define OGXM_BOARD RP2350_USB_A
// ----------------------------- //

#if   OGXM_BOARD == PI_PICO
    #define PIO_USB_DP_PIN  0          // D+ on GPIO0, D- on GPIO1
    #define SYS_CLOCK_KHZ   120000

#elif OGXM_BOARD == ADAFRUIT_FEATHER
    #define PIO_USB_DP_PIN  16         // D+ on GPIO16, D- on GPIO17
    #define VCC_EN_PIN      18         // Feather needs VBUS enabled on the host port
    #define SYS_CLOCK_KHZ   120000

#elif OGXM_BOARD == RP2350_USB_A
    #define PIO_USB_DP_PIN  12
    #define SYS_CLOCK_KHZ   240000     // RP2350 PIO USB host runs at 240 MHz

#else
    #error "No board selected"
#endif

// define pio config
#define PIO_USB_CONFIG {    \
    PIO_USB_DP_PIN,         \
    PIO_USB_TX_DEFAULT,     \
    PIO_SM_USB_TX_DEFAULT,  \
    PIO_USB_DMA_TX_DEFAULT, \
    PIO_USB_RX_DEFAULT,     \
    PIO_SM_USB_RX_DEFAULT,  \
    PIO_SM_USB_EOP_DEFAULT, \
    NULL,                   \
    PIO_USB_DEBUG_PIN_NONE, \
    PIO_USB_DEBUG_PIN_NONE, \
    false,                  \
    PIO_USB_PINOUT_DPDM }

extern void hid_app_task(void); // see hid_app.c

void usbh_task()
{
    #ifdef VCC_EN_PIN // Board needs VCC enabled on the USB host port
        gpio_init(VCC_EN_PIN);
        gpio_set_dir(VCC_EN_PIN, GPIO_OUT);
        gpio_put(VCC_EN_PIN, 1);
    #endif

    pio_usb_configuration_t pio_cfg = PIO_USB_CONFIG;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    tuh_init(BOARD_TUH_RHPORT);

    while (1)
    {
        tuh_task();
        hid_app_task(); // updates gamepad with controller data
    }
}

int main(void)
{
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // RP2350 timer debug-pause bug: timers can stall even outside a debugger
    // in some configurations, breaking the PIO USB SOF alarm.
    // https://forums.raspberrypi.com/viewtopic.php?t=363914
    timer_hw->dbgpause = 0;

    board_init();

    enum InputMode input_mode = INPUT_MODE_XINPUT; // choose an input mode

    init_tusb_gamepad(input_mode); // initialize usb device with chosen input mode

    multicore_reset_core1();
    multicore_launch_core1(usbh_task); // usb host stack on core 1

    while (1)
    {
        tud_task();
        tusb_gamepad_task();
    }

    return 0;
}
