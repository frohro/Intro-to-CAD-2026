cmake_minimum_required(VERSION 3.13)

# Include Pico SDK cmake file
include(pico_sdk_import.cmake)

project(cjc4334h_dac_test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(cjc4334h_dac_test
    main.c
)

# Generate PIO header file from i2s_tx.pio
pico_generate_pio_header(cjc4334h_dac_test ${CMAKE_CURRENT_LIST_DIR}/i2s_tx.pio)

# Pull in stdlib, PIO, DMA, clocks, PWM libraries
target_link_libraries(cjc4334h_dac_test
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_pwm
)

# Enable USB CDC terminal output & input
pico_enable_stdio_usb(cjc4334h_dac_test 1)
pico_enable_stdio_uart(cjc4334h_dac_test 0)

# Create map, bin, hex, uf2 files
pico_add_extra_outputs(cjc4334h_dac_test)
