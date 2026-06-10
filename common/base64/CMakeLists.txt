# Header-only base64 decoder shared by parser_protobuf and
# data_stream_foxglove_bridge. Internal INTERFACE target — not exported, not
# installed; consumers link `pj_base64` and #include <pj_base64/base64.hpp>.
#
# Always configurable from the top-level CMakeLists.txt (no external deps), so it
# is available regardless of whether the build picks a single plugin or all.

add_library(pj_base64 INTERFACE)
target_include_directories(pj_base64 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_base64 INTERFACE cxx_std_20)
