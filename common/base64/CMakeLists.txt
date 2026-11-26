# Header-only base64 decoder shared by parser_protobuf and data_stream_foxglove_bridge.
# No external deps, so it configures in single-plugin and aggregate builds alike.

add_library(pj_base64 INTERFACE)
target_include_directories(pj_base64 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_base64 INTERFACE cxx_std_20)
