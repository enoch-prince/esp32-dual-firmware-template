#!/bin/bash

export NON_INTERACTIVE=1
set -euo pipefail  # Exit on error + verbose


# ============================================
# 1. ACTIVATE ESP-IDF ENVIRONMENT
# ============================================
# echo "idf.py location: $(which python)"
IDF_PY="/home/epk/.espressif/tools/python/v6.0/venv/bin/python /home/epk/.espressif/v6.0/esp-idf/tools/idf.py"


echo "IDF Version:"
$IDF_PY --version

# if [ ! -f "$IDF_PY" ]; then
#     echo "Error: idf.py not found at $IDF_PY"
#     exit 1
# fi

# ============================================
# 2. VERIFY idf.py IS AVAILABLE
# ============================================
# if ! command -v idf.py &> /dev/null; then
#     echo "Warning: idf.py not found in PATH after sourcing."
#     echo "Falling back to explicit path..."
#     IDF_PY="$HOME/.espressif/tools/idf.py"
#     if [ -f "$IDF_PY" ]; then
#         alias idf.py="$IDF_PY"
#     else
#         echo "Error: idf.py not found anywhere."
#         exit 1
#     fi
# fi



# ============================================
# 3. SET DEFAULT FLAGS
# ============================================
FLASH=true
BUILD_A=true
BUILD_B=true

# ============================================
# 4. PARSE ARGUMENTS
# ============================================
for arg in "$@"; do
    case $arg in
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --build-only   Skip flashing, only build the project"
            echo "  --fw-a         Build firmware A only (skip B)"
            echo "  --fw-b         Build firmware B only (skip A)"
            echo "  --help         Show this help message"
            exit 0
            ;;
        --build-only) FLASH=false ;;
        --fw-a)       BUILD_B=false ;;
        --fw-b)       BUILD_A=false ;;
        *)            echo "Error: Unknown option '$arg'"; exit 1 ;;
    esac
done

# ============================================
# 5. MAIN LOGIC
# ============================================
echo "Configuration:"
echo "  Flash: $FLASH"
echo "  Build A: $BUILD_A"
echo "  Build B: $BUILD_B"
echo ""

if [ "$BUILD_A" = true ]; then
    echo "Building Firmware A..."
    $IDF_PY build --help
    if [ "$FLASH" = true ]; then
        echo "Flashing Firmware A..."
        $IDF_PY flash --help
    fi
fi

if [ "$BUILD_B" = true ]; then
    echo "Building Firmware B..."
    $IDF_PY build --help
    if [ "$FLASH" = true ]; then
        echo "Flashing Firmware B..."
        $IDF_PY flash --help
    fi
fi

echo "Done!"