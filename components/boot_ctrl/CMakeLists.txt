idf_component_register(
    SRCS
        "boot_ctrl.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        expected
        nvs_flash        # nvs_open, nvs_get_u8 etc.
        app_update       # esp_ota_ops.h
        esp_partition    # esp_partition_find_first
        # esp_log          # ESP_LOGI etc.  (was 'log' in IDF v4)
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