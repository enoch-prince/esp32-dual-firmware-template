idf_component_register(
    INCLUDE_DIRS "include"
    # No SRCS needed for header-only component
    # No REQUIRES for a pure header library
)

# Ensure C++17 is used when this component is included
target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_17)

# Optional: A compile definition to help with debugging
target_compile_definitions(${COMPONENT_LIB} INTERFACE 
    ESP_EXPECTED_POLYFILL=1
)