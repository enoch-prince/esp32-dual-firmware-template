#!/usr/bin/env bash
# build_and_flash.sh
#
# Builds both firmware images and flashes them to the correct partitions.
#
# Usage:
#   ./build_and_flash.sh                  # build + flash both
#   ./build_and_flash.sh --build-only     # build only, no flash
#   ./build_and_flash.sh --fw-a           # build + flash Firmware A only
#   ./build_and_flash.sh --fw-b           # build + flash Firmware B only
#
# Prerequisites:
#   - ESP-IDF environment sourced  (. $IDF_PATH/export.sh)
#   - Device connected via USB


# echo "running command: source $HOME/.espressif/tools/activate_idf_v6.0.sh"
# source "$HOME/.espressif/tools/activate_idf_v6.0.sh"
# echo "--------------"



set -euo pipefail

PORT="${ESPPORT:-/dev/ttyUSB0}"
BAUD="${ESPBAUD:-921600}"
PYTHON="$HOME/.espressif/tools/python/v6.0/venv/bin/python"
IDF_PY="$PYTHON $HOME/.espressif/v6.0/esp-idf/tools/idf.py"

BUILD_A=true
BUILD_B=true
FLASH=true

for arg in "$@"; do
    case $arg in
        --help) 
            echo "╔══════════════════════════════════╗"
            echo "║               HELP               ║"
            echo "╚══════════════════════════════════╝"
            echo " Builds both firmware images and flashes them to the correct partitions."
            echo ""
            echo " Usage:"
            echo "   ./build_and_flash.sh                  # build + flash both"
            echo "   ./build_and_flash.sh --build-only     # build only, no flash"
            echo "   ./build_and_flash.sh --fw-a           # build + flash Firmware A only"
            echo "   ./build_and_flash.sh --fw-b           # build + flash Firmware B only"
            echo ""
            echo " Prerequisites:"
            echo "   - ESP-IDF environment sourced  (. $IDF_PATH/export.sh)"
            echo "   - Device connected via USB"        
            exit 0
          ;;
        --build-only) FLASH=false ;;
        --fw-a)       BUILD_B=false ;;
        --fw-b)       BUILD_A=false ;;
        *)            echo "Error: Unknown option '$arg'"; exit 1 ;;
    esac
done

echo "╔══════════════════════════╗"
echo "║      CONFIGURATION       ║"
echo "╚══════════════════════════╝"
echo "  Flash: $FLASH"
echo "  Build A: $BUILD_A"
echo "  Build B: $BUILD_B"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "$SCRIPT_DIR"
cd "$SCRIPT_DIR"

# ── Partition offsets (must match partitions/partitions.csv) ──────────────
OTA_0_OFFSET="0x10000"
OTA_1_OFFSET="0x1D0000"
PARTITION_OFFSET="0x8000"
BOOTLOADER_OFFSET="0x1000"

# ── Build Firmware A ──────────────────────────────────────────────────────
if $BUILD_A; then
    echo ""
    echo "╔══════════════════════════════════╗"
    echo "║  Building Firmware A (ota_0)     ║"
    echo "╚══════════════════════════════════╝"
    $IDF_PY \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_a" \
        -B build_fw_a \
        build
fi

# ── Build Firmware B ──────────────────────────────────────────────────────
if $BUILD_B; then
    echo ""
    echo "╔══════════════════════════════════╗"
    echo "║  Building Firmware B (ota_1)     ║"
    echo "╚══════════════════════════════════╝"
    $IDF_PY \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_b" \
        -B build_fw_b \
        build
fi

if ! $FLASH; then
    echo ""
    echo "Build complete. Binaries:"
    $BUILD_A && echo "  Firmware A: build_fw_a/esp32s2_dual_fw.bin"
    $BUILD_B && echo "  Firmware B: build_fw_b/esp32s2_dual_fw.bin"
    exit 0
fi

# ── Flash everything ──────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════╗"
echo "║  Flashing to ${PORT}             ║"
echo "╚══════════════════════════════════╝"

# The bootloader and partition table only need to be flashed once.
# We use Firmware A's build for these.
FLASH_ARGS=(
    --port "$PORT"
    --baud "$BAUD"
    --before default_reset
    --after hard_reset
    write_flash
    --flash_mode dio
    --flash_freq 80m
    --flash_size 4MB
)

if $BUILD_A; then
    FLASH_ARGS+=(
        "${BOOTLOADER_OFFSET}"  "build_fw_a/bootloader/bootloader.bin"
        "${PARTITION_OFFSET}"   "build_fw_a/partition_table/partition-table.bin"
        "${OTA_0_OFFSET}"       "build_fw_a/esp32s2_dual_fw.bin"
    )
fi

if $BUILD_B; then
    FLASH_ARGS+=("${OTA_1_OFFSET}" "build_fw_b/esp32s2_dual_fw.bin")
fi

$PYTHON -m esptool "${FLASH_ARGS[@]}"

echo ""
echo "✓ Flash complete."
echo "  Firmware A @ ${OTA_0_OFFSET}  (active on first boot)"
echo "  Firmware B @ ${OTA_1_OFFSET}  (standby)"
