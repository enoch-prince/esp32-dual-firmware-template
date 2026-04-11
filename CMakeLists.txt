cmake_minimum_required(VERSION 3.16)

# ── Target ────────────────────────────────────────────────────────────────────
# Must be set before the IDF project.cmake include.
# Also set in sdkconfig.defaults, but CMake needs it here for the toolchain.
# set(IDF_TARGET "esp32s2")
set(IDF_TARGET "esp32")

# ── Custom bootloader ─────────────────────────────────────────────────────────
# Tells IDF to look in bootloader_components/ for a replacement bootloader main.
set(BOOTLOADER_EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/bootloader_components
)

# Accept firmware variant as build argument (Default: firmware_a)
if(NOT DEFINED FIRMWARE_VARIANT)
    set(FIRMWARE_VARIANT "firmware_a")
endif()

message(STATUS "Building firmware variant: ${FIRMWARE_VARIANT}")

# Validate variant exists
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${FIRMWARE_VARIANT}")
    message(FATAL_ERROR "Firmware variant '${FIRMWARE_VARIANT}' not found in ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()


# ── Extra component search paths ──────────────────────────────────────────────
# IDF needs to be explicitly told where our custom components live.
# ✅ Add selected firmware + shared components
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/${FIRMWARE_VARIANT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

# ✅ Exclude submodule source dirs from component scanning
set(COMPONENT_DIRS_EXCLUDE 
    "${CMAKE_CURRENT_SOURCE_DIR}/external/etl_src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/jsmn_src"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sample_project)

# ── C++17 ─────────────────────────────────────────────────────────────────────
# Apply after project() — idf_build_set_property requires the build system
# to be initialised first.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;" APPEND)

