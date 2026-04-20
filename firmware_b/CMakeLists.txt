idf_component_register(
    SRCS
        "app_main.cpp"
        "wifi.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        expected
        boot_ctrl
        ota_manager
        net_cmd
        health_monitor
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_driver_gpio
        mbedtls
        jsmn
        etl
    EMBED_TXTFILES
        certs/server_ca.pem
        certs/ecdsa_pub.pem
        certs/mqtt_ca.pem
        certs/client_cert.pem
        certs/client_key.pem
        certs/hmac_secret.txt
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

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
