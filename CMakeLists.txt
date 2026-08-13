cmake_minimum_required(VERSION 3.12)

message("Build type: \"${CMAKE_BUILD_TYPE}\"")

# Project name
set(NAME tusb_gamepad_macros)

set(PICO_BOARD none)
add_compile_definitions(PICO_BOOT_STAGE2_CHOOSE_GENERIC_03H=1 PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
# --- Target the RP2350 (Waveshare RP2350-USB-A) ---
# Must be set before the SDK init include below.
set(PICO_PLATFORM rp2350)

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(${NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.1.1")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 2.1.1 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

set(ROOT ${CMAKE_CURRENT_LIST_DIR})
set(PICO_PIO_USB_PATH ${ROOT}/lib/Pico-PIO-USB)
set(PICO_TINYUSB_PATH ${ROOT}/lib/tinyusb)
set(TUSB_GAMEPAD_PATH ${ROOT}/lib/tusb_gamepad)

pico_sdk_init()

add_subdirectory(${PICO_PIO_USB_PATH})
add_subdirectory(${TUSB_GAMEPAD_PATH})
target_include_directories(tusb_gamepad PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src) # so tusb_gamepad can see tusb_config.h
target_compile_options(tusb_gamepad PRIVATE -w) # suppress warnings from third-party submodule

file(GLOB_RECURSE SOURCES
    "src/main.c"
    "src/hid_app.c"
    "src/xinput_host.c"
    # Phase 4: macro engine port + on-flash profile store + config-mode CDC
    # server. Listed explicitly rather than as a bare src/*.c glob so a scratch
    # file dropped in src/ can never silently enter the build.
    "src/macro_engine.c"
    "src/aim_context.c"
    "src/profile_store.c"
    "src/config_mode.c"
    "src/boot_request.c"
    # Right-stick geometry correction (C++17 header-only StickGeometry.h,
    # bridged into the C build via this one translation unit).
    "src/stick_geometry_bridge.cpp")

add_executable(${NAME} ${SOURCES})

target_include_directories(${NAME} PUBLIC
    ${ROOT}/src
    ${ROOT}/lib)

target_link_libraries(${NAME}
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    # Phase 4 additions:
    #   hardware_flash - flash_range_erase/program for the profile store
    #   hardware_sync  - save_and_disable_interrupts around the flash write
    #   pico_rand      - get_rand_32() to seed the macro-jitter PRNG
    #   hardware_watchdog - warm-reboot + scratch registers for boot_request.c
    hardware_flash
    hardware_sync
    hardware_watchdog
    pico_rand
    tinyusb_board
    tinyusb_host
    tinyusb_device
    pico_pio_usb
    tusb_gamepad)

pico_add_extra_outputs(${NAME})
