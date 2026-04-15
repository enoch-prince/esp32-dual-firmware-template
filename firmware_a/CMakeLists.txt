
# Fetch expected (header-only, C++17 std::expected polyfill)
# include(FetchContent)

# FetchContent_Declare(
#     expected
#     GIT_REPOSITORY https://github.com/TartanLlama/expected.git
#     GIT_TAG        v1.3.1
#     GIT_SHALLOW    TRUE  # Faster clone
# )
# FetchContent_MakeAvailable(expected)
# # target_link_libraries(${COMPONENT_LIB} PRIVATE expected)
# # ✅ Add the include directory explicitly
# target_include_directories(${COMPONENT_LIB} PRIVATE 
#     ${expected_SOURCE_DIR}/include
# )

# Fetch ETL (Embedded Template Library)
# FetchContent_Declare(
#     etl
#     GIT_REPOSITORY https://github.com/ETLCPP/etl.git
#     GIT_TAG        20.47.1
#     GIT_SHALLOW    TRUE  # Faster clone
# )
# FetchContent_MakeAvailable(etl)
# target_link_libraries(${COMPONENT_LIB} PRIVATE etl::etl)


# # JSMN (header-only JSON parser, no malloc)
# FetchContent_Declare(
#     jsmn
#     GIT_REPOSITORY https://github.com/zserge/jsmn.git
#     GIT_TAG        master
#     GIT_SHALLOW    TRUE  # Faster clone
# )
# FetchContent_MakeAvailable(jsmn)

# # Add jsmn header to include path
# target_include_directories(${COMPONENT_LIB} PRIVATE ${jsmn_SOURCE_DIR})

idf_component_register(
    SRCS
        "app_main.cpp"
        "wifi.cpp"
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
        esp_driver_gpio
        mbedtls
        jsmn
        etl
        # esp_log
    EMBED_TXTFILES
        certs/server_ca.pem
        certs/ecdsa_pub.pem
        certs/mqtt_ca.pem
        certs/client_cert.pem
        certs/client_key.pem
        certs/hmac_secret.txt
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
