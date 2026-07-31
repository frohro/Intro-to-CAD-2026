cmake_minimum_required(VERSION 3.13)

# Research: 48/96 kHz Dual-Rate UAC1 SDR
#
# Extends the Lab 7 firmware with:
#   • Runtime 48/96 kHz switching via USB SET_CUR or CDC RATE command
#   • PCM1808 256fs mode (MD1=HIGH) for 96 kHz using the same 24.576 MHz crystal
#   • 3-byte (S24_3LE) USB subframe format — true 24-bit, 25% less bandwidth
#   • Both rates advertised in the UAC1 Type I Format descriptor
#
# USB interface layout (same as Lab 7):
#   IAD 1: Interface 0 — CDC Control (ACM, EP 0x83 IN notification)
#           Interface 1 — CDC Data   (EP 0x04 OUT, EP 0x84 IN, bulk 64 B)
#   IAD 2: Interface 2 — Audio Control   (no EPs)
#           Interface 3 — Audio Streaming (alt 0: zero-bw;
#                                          alt 1: EP 0x81 IN isochronous async)
#
# Hardware: Intro-to-CAD-2026 v0.1 board (YD-RP2040), DIRECT LO topology.
# A v0.2 board will add Johnson counter mode; update BOARD_DIRECT_MODE in main.c.
# I2C: GPIO12=SDA, GPIO13=SCL (Si5351a at 0x60, 24.576 MHz crystal)
# PIO: GPIO14=DATA, GPIO15=BCK, GPIO16=WS (PCM1808 I2S 24-bit master)
# MD1: GPIO11 — LOW=48 kHz (512fs), HIGH=96 kHz (256fs)

include(pico_sdk_import.cmake)

project(sdr_96k C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sdr_96k
    main.c
    si5351.c
    usb_descriptors.c
)

# Generate i2s_rx.pio.h from the PIO source so main.c can #include it.
pico_generate_pio_header(sdr_96k ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

# tusb_config.h and generated pio header are in the build/source directory.
target_include_directories(sdr_96k PRIVATE .)

target_link_libraries(sdr_96k
    pico_stdlib        # GPIO, sleep_ms, tight_loop_contents
    hardware_i2c       # i2c_init, i2c_read/write_blocking   (Si5351a)
    hardware_pio       # pio_add_program, pio_sm_init, etc.  (I2S RX)
    hardware_dma       # dma_channel_configure, dma_hw, etc. (ping-pong)
    pico_multicore     # multicore_launch_core1, multicore_fifo_*
    tinyusb_device     # TinyUSB device stack
    tinyusb_board      # BSP: board_init(), board_usb_get_serial()
)

# pico_enable_stdio_usb MUST be 0 when using TinyUSB directly.
pico_enable_stdio_usb(sdr_96k 0)
pico_enable_stdio_uart(sdr_96k 0)

# Produce sdr_96k.uf2, sdr_96k.bin, sdr_96k.hex alongside sdr_96k.elf
pico_add_extra_outputs(sdr_96k)
