cmake_minimum_required(VERSION 3.13)

# Research: 48/96/192 kHz Multi-ADC UAC1 SDR (v0.2)
#
# Unified firmware for the Intro-to-CAD-2026 v0.2 board (YD-RP2040).
# Enables runtime switching between PCM1808 and CJC5430 ADCs
# and multiple sample rates (48 kHz, 96 kHz, 192 kHz).

include(pico_sdk_import.cmake)

if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME})
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH "Path to TinyUSB")
endif()

project(sdr_2026_v0_2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_2026_v0_2
    main.c
    si5351.c
    usb_descriptors.c
)

# Generate i2s_rx.pio.h from the PIO source so main.c can #include it.
pico_generate_pio_header(sdr_2026_v0_2 ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

# tusb_config.h and generated pio header are in the build/source directory.
target_include_directories(sdr_2026_v0_2 PRIVATE .)

target_link_libraries(sdr_2026_v0_2
    pico_stdlib        # GPIO, sleep_ms, tight_loop_contents
    hardware_i2c       # i2c_init, i2c_read/write_blocking   (Si5351a)
    hardware_pio       # pio_add_program, pio_sm_init, etc.  (I2S RX)
    hardware_dma       # dma_channel_configure, dma_hw, etc. (ping-pong)
    pico_multicore     # multicore_launch_core1, multicore_fifo_*
    tinyusb_device     # TinyUSB device stack
    tinyusb_board      # BSP: board_init(), board_usb_get_serial()
)

# pico_enable_stdio_usb MUST be 0 when using TinyUSB directly.
pico_enable_stdio_usb(sdr_2026_v0_2 0)
pico_enable_stdio_uart(sdr_2026_v0_2 0)

# Produce sdr_2026_v0_2.uf2, sdr_2026_v0_2.bin, sdr_2026_v0_2.hex alongside sdr_2026_v0_2.elf
pico_add_extra_outputs(sdr_2026_v0_2)
