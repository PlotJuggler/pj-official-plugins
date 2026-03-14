find_package(nlohmann_json REQUIRED)
find_package(mcap REQUIRED)

# --- Plugin shared library ---
add_library(mcap_source_plugin SHARED src/mcap_source.cpp)
target_compile_features(mcap_source_plugin PRIVATE cxx_std_20)
target_compile_options(mcap_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mcap_source_plugin PRIVATE pj_base nlohmann_json::nlohmann_json mcap::mcap)
