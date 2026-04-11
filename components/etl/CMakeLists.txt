idf_component_register(
    INCLUDE_DIRS "../../external/etl_src/include"
    # ✅ CRITICAL: Do NOT add ETL's source dir to SRCS or let ESP-IDF process its CMakeLists.txt
    # Header-only: no compilation needed
)
target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_17)

# ✅ OPTIONAL: Suppress ETL's own CMake if it gets picked up
# set(ETL_CMAKE_SKIP TRUE)