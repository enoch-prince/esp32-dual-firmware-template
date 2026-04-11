idf_component_register(
    SRCS
        "ota_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        etl
        expected
        boot_ctrl
        app_update       # esp_ota_ops.h, esp_app_desc.h
        esp_https_ota    # esp_https_ota_begin etc.
        esp_http_client  # esp_http_client_init etc.
        esp_partition    # esp_partition_find_first, esp_partition_mmap
        jsmn             # manual json build
        # json             # cJSON
        log              # ESP_LOGx macros
        nvs_flash
        mbedtls          # PSA crypto headers for signature verification
)

# target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -fconcepts)
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
