cmake_minimum_required(VERSION 3.13)

# Research: 192 kHz UAC1 SDR (16-bit USB stream)
#
# Extends the Lab 7 firmware with:
#   • Fixed 192 kHz operation for the CJC5340 path
#   • 24-bit capture in PIO/DMA, dithered down to 16-bit over USB
#   • 2-byte (S16_LE) USB subframe format to fit full-speed USB bandwidth
#
# USB interface layout (same as Lab 7):
#   IAD 1: Interface 0 — CDC Control (ACM, EP 0x83 IN notification)
#           Interface 1 — CDC Data   (EP 0x04 OUT, EP 0x84 IN, bulk 64 B)
#   IAD 2: Interface 2 — Audio Control   (no EPs)
#           Interface 3 — Audio Streaming (alt 0: zero-bw;
#                                          alt 1: 192 kHz 768 B/ms)
#
# Hardware: Intro-to-CAD-2026 v0.2 board (YD-RP2040), Johnson counter LO.
# I2C: GPIO12=SDA, GPIO13=SCL (Si5351a at 0x60, 24.576 MHz crystal)
# PIO: GPIO6=DATA, GPIO7=BCK, GPIO8=WS (CJC5340 I2S 24-bit master)
# RST: Wired HIGH on v0.2. Mode set by JP_M0/JP_M1 jumpers.

include(pico_sdk_import.cmake)

# These firmware sources use TinyUSB UAC1 descriptor macros from the
# standalone TinyUSB checkout rather than the Pico SDK bundled copy.
if(NOT DEFINED PICO_TINYUSB_PATH AND DEFINED ENV{HOME})
    set(PICO_TINYUSB_PATH "$ENV{HOME}/tinyusb" CACHE PATH "Path to TinyUSB")
endif()

project(sdr_192k C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_192k
    main.c
    si5351.c
    usb_descriptors.c
)

# Generate i2s_rx.pio.h from the PIO source so main.c can #include it.
pico_generate_pio_header(sdr_192k ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

# tusb_config.h and generated pio header are in the build/source directory.
target_include_directories(sdr_192k PRIVATE .)

target_link_libraries(sdr_192k
    pico_stdlib        # GPIO, sleep_ms, tight_loop_contents
    hardware_i2c       # i2c_init, i2c_read/write_blocking   (Si5351a)
    hardware_pio       # pio_add_program, pio_sm_init, etc.  (I2S RX)
    hardware_dma       # dma_channel_configure, dma_hw, etc. (ping-pong)
    pico_multicore     # multicore_launch_core1, multicore_fifo_*
    tinyusb_device     # TinyUSB device stack
    tinyusb_board      # BSP: board_init(), board_usb_get_serial()
)

# pico_enable_stdio_usb MUST be 0 when using TinyUSB directly.
pico_enable_stdio_usb(sdr_192k 0)
pico_enable_stdio_uart(sdr_192k 0)

# Produce sdr_192k.uf2, sdr_192k.bin, sdr_192k.hex alongside sdr_192k.elf
pico_add_extra_outputs(sdr_192k)
