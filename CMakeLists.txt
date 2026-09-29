cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2040_zero CACHE STRING "Board type")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico-usb-nak C CXX ASM)
pico_sdk_init()

add_executable(pico-usb-nak main.c)

pico_generate_pio_header(pico-usb-nak ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(pico-usb-nak
    pico_stdlib
    hardware_pio
    hardware_resets
    hardware_watchdog
)

# USB stdio is linked but NOT started automatically:
# main() calls stdio_usb_init() only in NORMAL mode.
pico_enable_stdio_usb(pico-usb-nak 1)
pico_enable_stdio_uart(pico-usb-nak 1)

# Optional external mode switch (GND = start in UNRESP):
# target_compile_definitions(pico-usb-nak PRIVATE MODE_SWITCH_PIN=29)

pico_add_extra_outputs(pico-usb-nak)
