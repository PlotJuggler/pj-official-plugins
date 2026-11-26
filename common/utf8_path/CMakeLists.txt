# Header-only UTF-8 <-> std::filesystem::path helpers shared by every plugin that opens a file
# the host named (a UTF-8 std::string). Internal INTERFACE target, not exported, not installed;
# consumers link `pj_utf8_path` and #include <pj_utf8_path/utf8_path.hpp>.
#
# Always configurable from the top-level CMakeLists.txt (no external deps).

add_library(pj_utf8_path INTERFACE)
target_include_directories(pj_utf8_path INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_utf8_path INTERFACE cxx_std_20)

# GTest-guarded so this common target can be configured unconditionally.
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_utf8_path_test tests/utf8_path_test.cpp)
  target_compile_features(pj_utf8_path_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_utf8_path_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_utf8_path_test PRIVATE pj_utf8_path GTest::gtest_main)
  add_test(NAME pj_utf8_path_test COMMAND pj_utf8_path_test)
endif()
