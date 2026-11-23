cmake_minimum_required(VERSION 3.13)

# Unified USB + WiFi OpenHPSDR SDR Firmware for Raspberry Pi Pico W / Pico 2 W (CYW43439)
# Targets Intro-to-CAD-2026 v0.1 Board
#
# Supports:
#   • Target Boards: Pico W (RP2040) or Pico 2 W (RP2350) via -DPICO_BOARD=pico_w / -DPICO_BOARD=pico2_w
#   • USB Audio Class 1.0 (UAC1 48k/96k 24-bit)
#   • USB CDC serial control
#   • WiFi OpenHPSDR Protocol 1 (UDP Port 1024)
#   • WiFi TCP Control Server (TCP Port 5000)
#   • On-board Si5351a Golden LO calculation & direct tuning

# Default to pico_w if not specified on command line (-DPICO_BOARD=pico2_w or -DPICO_BOARD=pico_w)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Target board (pico_w or pico2_w)")
endif()

include(pico_sdk_import.cmake)

if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME})
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH "Path to TinyUSB")
endif()

project(sdr_v0.1_unified C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_v0.1_unified
    main.c
    si5351.c
    openhpsdr.c
    usb_descriptors.c
)

pico_generate_pio_header(sdr_v0.1_unified ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

target_include_directories(sdr_v0.1_unified PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_TINYUSB_PATH}/hw/bsp
    ${PICO_TINYUSB_PATH}/src
)

target_link_libraries(sdr_v0.1_unified
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_vreg
    hardware_flash
    hardware_sync
    pico_multicore
    tinyusb_device
    tinyusb_board
)

pico_enable_stdio_usb(sdr_v0.1_unified 0)
pico_enable_stdio_uart(sdr_v0.1_unified 1)

pico_add_extra_outputs(sdr_v0.1_unified)
