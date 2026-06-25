# tusb_gamepad_xbox_to_xinput_pico

Have an Xbox controller (or a compatible third-party controller) that you want to use on your PC or console, but need a custom middleman? This project turns a Raspberry Pi Pico into a smart USB adapter. 

It reads the signals from your connected controller, translates them, and sends them to your PC as a standard, universally recognized XInput device. 

If you have any tips or want to make an PR, please do
---

## What can you do with this?

* **Play instantly:** Connect an Xbox controller/third party conntroller to your Pico, plug the Pico into your PC, and it will be recognized as an official XInput controller.
* **Zero lag:** Built on the fast RP2040/RP2350 microcontrollers, meaning your button presses are registered instantly with no noticeable delay.
* **Custom projects:** Perfect if you want to build custom arcade sticks, modify older controllers, or map inputs for emulators and modern PC games.

---

## How does it work?

Think of the Pico as a translator sitting between your controller and your PC:
1. **Listen:** The Pico acts as a "Host" and reads the raw data (button presses, joystick movements) coming from your Xbox controller.
2. **Translate:** The software inside the Pico converts those signals into the standard XInput format that Windows and games require.
3. **Speak:** The Pico acts as a "Device" and sends that translated data straight to your PC.

---

## What do you need?

### Hardware
* **Raspberry Pi Pico board** (Note: This project is tested on boards with a built-in/soldered USB-A port, such as the RP2350_USB_A, which feature both USB-C and USB-A ports for convenience).
* **USB OTG Adapter** (Micro-USB/USB-C to USB-A Female) if your specific Pico board does not have a built-in USB-A port.
* **An Xbox Controller** (or a compatible third-party controller).
* **A USB cable** to connect your Pico to your PC.

### Compiling the code
To build the project from source, you will need the following tools:
* **Pico SDK** (version 2.1.1 or higher recommended).
* **TinyUSB stack** (included with the Pico SDK).
* **CMake** and **GCC ARM Compiler** (or VS Code with the official Raspberry Pi Pico extension).

---

## Building and Installation

Follow these steps in your terminal or command prompt to set up the Pico SDK, clone the repository, and build the firmware.

### 1. Setup the Pico SDK
Clone the required version of the Pico SDK, initialize its submodules, and set the environment path:

```bash
git clone --branch 2.1.1 --depth 1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk-right
cd ~/pico-sdk-right && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk-right

### 2. Build the project
git clone --recursive https://github.com/aleksia123/tusb_gamepad_xbox_to_xinput_pico.git
cd tusb_gamepad_xbox_to_xinput_pico
mkdir -p build
cd build
cmake ..
make -j$(nproc)

## Acknowledgements

This project is built on top of the following open-source tools:
* [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) - The foundational framework for RP2040/RP2350 development.
* [TinyUSB](https://github.com/hathach/tinyusb) - The embedded USB stack handling both host controller reading and device communication.
