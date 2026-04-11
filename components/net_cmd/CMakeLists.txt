idf_component_register(
    SRCS
        "http_cmd.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        boot_ctrl
        ota_manager
        esp_http_server
        # esp_tls           # ✅ For HTTPS/TLS support
        # esp_log           # ESP_LOGI etc. (correct name in IDF v5+)
        # json              # cJSON
        etl
        jsmn
        mbedtls          # PSA crypto headers for HMAC verification
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