idf_component_register(
    SRCS
        "http_cmd.cpp"
        # mqtt_cmd.cpp excluded: espressif/esp-mqtt not available in IDF v6 local env.
        # Re-add once component registry is configured:
        #   idf.py add-dependency "espressif/esp-mqtt>=2.0.0"
    INCLUDE_DIRS
        "include"
    REQUIRES
        boot_ctrl
        ota_manager
        esp_http_server
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