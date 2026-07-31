cmake_minimum_required(VERSION 3.13)

# Research: 48/96 kHz Dual-Rate UAC1 SDR
#
# Extends the Lab 7 firmware with:
#   • Runtime 48/96 kHz switching via USB SET_CUR or CDC RATE command
#   • CJC5340 support (48 kHz/96 kHz) using 24.576 MHz SCKI
#   • 3-byte (S24_3LE) USB subframe format — true 24-bit, 25% less bandwidth
#   • Both rates advertised in the UAC1 Type I Format descriptor
#
# USB interface layout (same as Lab 7):
#   IAD 1: Interface 0 — CDC Control (ACM, EP 0x83 IN notification)
#           Interface 1 — CDC Data   (EP 0x04 OUT, EP 0x84 IN, bulk 64 B)
#   IAD 2: Interface 2 — Audio Control   (no EPs)
#           Interface 3 — Audio Streaming (alt 0: zero-bw;
#                                          alt 1: 48 kHz 288 B/ms;
#                                          alt 2: 96 kHz 576 B/ms)
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

project(sdr_48k C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_48k
    main.c
    si5351.c
    usb_descriptors.c
)

# Generate i2s_rx.pio.h from the PIO source so main.c can #include it.
pico_generate_pio_header(sdr_48k ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

# tusb_config.h and generated pio header are in the build/source directory.
target_include_directories(sdr_48k PRIVATE .)

target_link_libraries(sdr_48k
    pico_stdlib        # GPIO, sleep_ms, tight_loop_contents
    hardware_i2c       # i2c_init, i2c_read/write_blocking   (Si5351a)
    hardware_pio       # pio_add_program, pio_sm_init, etc.  (I2S RX)
    hardware_dma       # dma_channel_configure, dma_hw, etc. (ping-pong)
    pico_multicore     # multicore_launch_core1, multicore_fifo_*
    tinyusb_device     # TinyUSB device stack
    tinyusb_board      # BSP: board_init(), board_usb_get_serial()
)

# pico_enable_stdio_usb MUST be 0 when using TinyUSB directly.
pico_enable_stdio_usb(sdr_48k 0)
pico_enable_stdio_uart(sdr_48k 0)

# Produce sdr_48k.uf2, sdr_48k.bin, sdr_48k.hex alongside sdr_48k.elf
pico_add_extra_outputs(sdr_48k)
