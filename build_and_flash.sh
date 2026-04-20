#!/usr/bin/env bash
# build_and_flash.sh
#
# Builds both firmware images and flashes them to the correct partitions.
#
# Usage:
#   ./build_and_flash.sh                    # build + flash both
#   ./build_and_flash.sh --build-only       # build only, no flash
#   ./build_and_flash.sh --fw-a             # build + flash Firmware A (+ bootloader)
#   ./build_and_flash.sh --fw-b             # build + flash Firmware B only
#   ./build_and_flash.sh --flash-only       # skip build, flash everything
#   ./build_and_flash.sh --flash-fw-a       # skip build, flash Firmware A binary only (ota_0)
#   ./build_and_flash.sh --flash-fw-b       # skip build, flash Firmware B binary only (ota_1)
#   ./build_and_flash.sh --flash-bootloader # skip build, flash bootloader + partition table + ota_data
#
# Prerequisites:
#   - ESP-IDF environment sourced  (source "$HOME/.espressif/tools/activate_idf_v6.0.sh")
#   - Device connected via USB

set -euo pipefail

PORT="${ESPPORT:-/dev/ttyUSB0}"
BAUD="${ESPBAUD:-921600}"
PYTHON="$HOME/.espressif/tools/python/v6.0/venv/bin/python"
IDF_PY="$PYTHON $HOME/.espressif/v6.0/esp-idf/tools/idf.py"

# ── Build targets ──────────────────────────────────────────────────────────
BUILD_A=true
BUILD_B=true
FLASH=true

# ── Flash targets (empty = derive from build flags after arg parsing) ──────
# Set explicitly by --flash-* flags; derived from BUILD_* otherwise.
FLASH_BOOT=""    # bootloader + partition table + ota_data_initial
FLASH_APP_A=""   # ota_0  (Firmware A binary)
FLASH_APP_B=""   # ota_1  (Firmware B binary)

# ── Argument parsing ───────────────────────────────────────────────────────
for arg in "$@"; do
    case $arg in
        --help)
            echo "╔══════════════════════════════════════════════╗"
            echo "║                    HELP                      ║"
            echo "╚══════════════════════════════════════════════╝"
            echo ""
            echo " Builds firmware images and flashes them to the correct partitions."
            echo ""
            echo " Usage:"
            echo "   ./build_and_flash.sh                    # build + flash both"
            echo "   ./build_and_flash.sh --build-only       # build only, no flash"
            echo "   ./build_and_flash.sh --fw-a             # build + flash Firmware A (+ bootloader)"
            echo "   ./build_and_flash.sh --fw-b             # build + flash Firmware B only"
            echo "   ./build_and_flash.sh --flash-only       # skip build, flash everything"
            echo "   ./build_and_flash.sh --flash-fw-a       # skip build, flash FW-A binary only"
            echo "   ./build_and_flash.sh --flash-fw-b       # skip build, flash FW-B binary only"
            echo "   ./build_and_flash.sh --flash-bootloader # skip build, flash bootloader + partitions"
            echo ""
            echo " Environment overrides:"
            echo "   ESPPORT=/dev/ttyUSB1 ./build_and_flash.sh"
            echo "   ESPBAUD=460800       ./build_and_flash.sh"
            echo ""
            echo " Prerequisites:"
            echo "   source \"\$HOME/.espressif/tools/activate_idf_v6.0.sh\""
            echo ""
            $IDF_PY --version 2>&1 || true
            exit 0
            ;;
        --build-only)
            FLASH=false
            ;;
        --fw-a)
            # Build + flash Firmware A (with bootloader); skip Firmware B
            BUILD_B=false
            ;;
        --fw-b)
            # Build + flash Firmware B only; skip Firmware A and bootloader
            BUILD_A=false
            ;;
        --flash-only)
            # Skip build; flash everything that was previously built
            BUILD_A=false
            BUILD_B=false
            FLASH_BOOT=true
            FLASH_APP_A=true
            FLASH_APP_B=true
            ;;
        --flash-fw-a)
            # Skip build; flash Firmware A binary only (ota_0 — no bootloader/partition)
            BUILD_A=false
            BUILD_B=false
            FLASH_BOOT=false
            FLASH_APP_A=true
            FLASH_APP_B=false
            ;;
        --flash-fw-b)
            # Skip build; flash Firmware B binary only (ota_1)
            BUILD_A=false
            BUILD_B=false
            FLASH_BOOT=false
            FLASH_APP_A=false
            FLASH_APP_B=true
            ;;
        --flash-bootloader)
            # Skip build; flash bootloader + partition table + ota_data only
            BUILD_A=false
            BUILD_B=false
            FLASH_BOOT=true
            FLASH_APP_A=false
            FLASH_APP_B=false
            ;;
        *)
            echo "Error: Unknown option '$arg'" >&2
            echo "Run  ./build_and_flash.sh --help  for usage." >&2
            exit 1
            ;;
    esac
done

# ── Derive flash targets from build flags when not set explicitly ──────────
# (Applies to: no args, --fw-a, --fw-b)
if [ -z "$FLASH_BOOT" ]; then
    # Bootloader is only (re-)flashed when Firmware A is being written,
    # since we use build_fw_a for the bootloader and partition table.
    $BUILD_A && FLASH_BOOT=true || FLASH_BOOT=false
    $BUILD_A && FLASH_APP_A=true || FLASH_APP_A=false
    $BUILD_B && FLASH_APP_B=true || FLASH_APP_B=false
fi

# ── Configuration summary ──────────────────────────────────────────────────
echo "╔══════════════════════════════════════════════╗"
echo "║               CONFIGURATION                  ║"
echo "╚══════════════════════════════════════════════╝"
printf "  Port              : %s\n"    "$PORT"
printf "  Baud              : %s\n"    "$BAUD"
printf "  Build FW-A        : %s\n"    "$BUILD_A"
printf "  Build FW-B        : %s\n"    "$BUILD_B"
printf "  Flash             : %s\n"    "$FLASH"
printf "  Flash bootloader  : %s\n"    "$FLASH_BOOT"
printf "  Flash FW-A (ota_0): %s\n"    "$FLASH_APP_A"
printf "  Flash FW-B (ota_1): %s\n"    "$FLASH_APP_B"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Partition offsets (must match partitions/partitions.csv) ──────────────
BOOTLOADER_OFFSET="0x1000"    # always 0x1000 on ESP32
PARTITION_OFFSET="0x18000"    # CONFIG_PARTITION_TABLE_OFFSET in sdkconfig.defaults
OTA_DATA_OFFSET="0x1e000"     # otadata partition in partitions.csv
OTA_0_OFFSET="0x20000"        # ota_0 (Firmware A)
OTA_1_OFFSET="0x1E0000"       # ota_1 (Firmware B)

BIN_A="build_fw_a/sample_project.bin"
BIN_B="build_fw_b/sample_project.bin"

# ── Build Firmware A ──────────────────────────────────────────────────────
if $BUILD_A; then
    echo ""
    echo "╔══════════════════════════════════════════════╗"
    echo "║         Building Firmware A  (ota_0)         ║"
    echo "╚══════════════════════════════════════════════╝"
    $IDF_PY \
        -DFIRMWARE_VARIANT=firmware_a \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_a" \
        -B build_fw_a \
        build
fi

# ── Build Firmware B ──────────────────────────────────────────────────────
if $BUILD_B; then
    echo ""
    echo "╔══════════════════════════════════════════════╗"
    echo "║         Building Firmware B  (ota_1)         ║"
    echo "╚══════════════════════════════════════════════╝"
    $IDF_PY \
        -DFIRMWARE_VARIANT=firmware_b \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.fw_b" \
        -B build_fw_b \
        build
fi

# ── Build-only exit ────────────────────────────────────────────────────────
if ! $FLASH; then
    echo ""
    echo "╔══════════════════════════════════════════════╗"
    echo "║              Build complete                   ║"
    echo "╚══════════════════════════════════════════════╝"
    $BUILD_A && echo "  Firmware A : $BIN_A"
    $BUILD_B && echo "  Firmware B : $BIN_B"
    exit 0
fi

# ── Pre-flash binary existence checks ─────────────────────────────────────
missing=false
if $FLASH_BOOT && [ ! -f "build_fw_a/bootloader/bootloader.bin" ]; then
    echo "Error: build_fw_a/bootloader/bootloader.bin not found." >&2
    echo "       Run without --flash-* flags to build first, or use --fw-a." >&2
    missing=true
fi
if $FLASH_BOOT && [ ! -f "build_fw_a/partition_table/partition-table.bin" ]; then
    echo "Error: build_fw_a/partition_table/partition-table.bin not found." >&2
    missing=true
fi
if $FLASH_APP_A && [ ! -f "$BIN_A" ]; then
    echo "Error: $BIN_A not found.  Build FW-A first." >&2
    missing=true
fi
if $FLASH_APP_B && [ ! -f "$BIN_B" ]; then
    echo "Error: $BIN_B not found.  Build FW-B first." >&2
    missing=true
fi
$missing && exit 1

# ── Flash ─────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║           Flashing to ${PORT}           ║"
echo "╚══════════════════════════════════════════════╝"

FLASH_ARGS=(
    --port  "$PORT"
    --baud  "$BAUD"
    --before default-reset
    --after  hard-reset
    write-flash
    --flash-mode dio
    --flash-freq 80m
    --flash-size 4MB
)

if $FLASH_BOOT; then
    FLASH_ARGS+=(
        "$BOOTLOADER_OFFSET" "build_fw_a/bootloader/bootloader.bin"
        "$PARTITION_OFFSET"  "build_fw_a/partition_table/partition-table.bin"
        "$OTA_DATA_OFFSET"   "build_fw_a/ota_data_initial.bin"
    )
fi

if $FLASH_APP_A; then
    FLASH_ARGS+=("$OTA_0_OFFSET" "$BIN_A")
fi

if $FLASH_APP_B; then
    FLASH_ARGS+=("$OTA_1_OFFSET" "$BIN_B")
fi

$PYTHON -m esptool "${FLASH_ARGS[@]}"

# ── Summary ────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║              Flash complete                   ║"
echo "╚══════════════════════════════════════════════╝"
$FLASH_BOOT  && echo "  Bootloader     @ $BOOTLOADER_OFFSET"
$FLASH_BOOT  && echo "  Partition table @ $PARTITION_OFFSET"
$FLASH_BOOT  && echo "  OTA data        @ $OTA_DATA_OFFSET"
$FLASH_APP_A && echo "  Firmware A      @ $OTA_0_OFFSET  ← active on first boot"
$FLASH_APP_B && echo "  Firmware B      @ $OTA_1_OFFSET  ← standby"
