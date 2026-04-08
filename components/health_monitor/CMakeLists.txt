idf_component_register(
    SRCS
        "health_monitor.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        boot_ctrl
        # esp_log
        esp_timer        # not directly used but pulled in by FreeRTOS timers
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

# Embedded-friendly flags
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Os
    -ffunction-sections
    -fdata-sections
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -Wall
    -Wextra
)