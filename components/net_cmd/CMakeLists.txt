idf_component_register(
    SRCS
        "http_cmd.cpp"
        "mqtt_cmd.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        boot_ctrl
        ota_manager
        # esp_http_server   # httpd_*
        # esp_mqtt              # esp_mqtt_client — enabled via CONFIG_MQTT_ENABLED=y
        # esp_tls           # ✅ For HTTPS/TLS support
        # esp_log           # ESP_LOGI etc. (correct name in IDF v5+)
        # json              # cJSON
        # jsmn
        mbedtls           # HMAC-SHA256
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