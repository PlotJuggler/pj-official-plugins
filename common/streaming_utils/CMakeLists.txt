# Header-only primitives shared by streaming plugins. Keep this target free of
# protocol dependencies: consumers already link their own socket/MQTT/ROS SDKs.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(pj_streaming_utils INTERFACE)
target_include_directories(pj_streaming_utils INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(pj_streaming_utils INTERFACE cxx_std_20)
target_link_libraries(pj_streaming_utils INTERFACE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

add_executable(streaming_utils_test tests/streaming_utils_test.cpp)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(streaming_utils_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
target_link_libraries(streaming_utils_test PRIVATE
  pj_streaming_utils
  GTest::gtest_main
)
add_test(NAME streaming_utils_test COMMAND streaming_utils_test)
