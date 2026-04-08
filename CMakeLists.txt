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

# ── Extra component search paths ──────────────────────────────────────────────
# IDF needs to be explicitly told where our custom components live.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(sample_project)

# ── C++17 ─────────────────────────────────────────────────────────────────────
# Apply after project() — idf_build_set_property requires the build system
# to be initialised first.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;" APPEND)

