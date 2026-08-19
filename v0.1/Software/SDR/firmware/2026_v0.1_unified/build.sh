#!/usr/bin/env bash
#
# Build script for Unified Intro-to-CAD-2026 v0.1 SDR Firmware
# Supports building for either Raspberry Pi Pico W or Pico 2 W
#
# Usage:
#   ./build.sh          (defaults to pico_w)
#   ./build.sh pico_w   (builds for Raspberry Pi Pico W  - RP2040)
#   ./build.sh pico2_w  (builds for Raspberry Pi Pico 2 W - RP2350)
#

set -e

BOARD="${1:-pico_w}"

if [ "$BOARD" != "pico_w" ] && [ "$BOARD" != "pico2_w" ]; then
    echo "Usage: $0 [pico_w|pico2_w]"
    echo "  pico_w  : Raspberry Pi Pico W (RP2040)"
    echo "  pico2_w : Raspberry Pi Pico 2 W (RP2350)"
    exit 1
fi

echo "============================================================"
echo " Building Unified SDR Firmware (v0.1 Board)"
echo " Target Board: ${BOARD}"
echo "============================================================"

if [ -z "$PICO_TINYUSB_PATH" ] && [ -d "$HOME/tinyusb" ]; then
    export PICO_TINYUSB_PATH="$HOME/tinyusb"
fi

BUILD_DIR="build_${BOARD}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DPICO_BOARD="$BOARD" ..
make -j"$(nproc)"

echo "============================================================"
echo " Build Complete!"
echo " Output UF2: ${BUILD_DIR}/sdr_v0.1_unified.uf2"
echo "============================================================"
