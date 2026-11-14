# Shared CAN/DBC decoder (pj_can_dbc): turns raw CAN frames (id, extended, bytes)
# into named DBC signals, plus portable dictionary paths (dictionary_paths.hpp).
# Consumed by data_load_mf4, data_load_blf and data_load_candump.
#
# dbc_parser_cpp (+ the header-only fast_float) is vendored via CPM DOWNLOAD_ONLY
# and built as a static `dbc` lib here; it is PRIVATE-linked and fully hidden
# behind can_decoder's pImpl, so consumers never see a DBC header. Both deps are
# pinned by commit tarball + SHA256. Handles little- and big-endian signals.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/CPM.cmake)

CPMAddPackage(
  NAME fast_float
  URL https://github.com/fastfloat/fast_float/archive/1ea4f27b2aeee2859a1354a3c24cff52a116cad1.tar.gz
  URL_HASH SHA256=06f860e3adb994b9c8523bb53a76a335c4182e27fe5a077246146d283efdc9da
  DOWNLOAD_ONLY YES
)
CPMAddPackage(
  NAME dbc_parser_cpp
  URL https://github.com/LinuxDevon/dbc_parser_cpp/archive/2c8acdbd2d1ea33545f99f9a0d23e10b7f8e0852.tar.gz
  URL_HASH SHA256=91e12166d83c56f1ff945f6c94f14a7fe80e105219111f984645614ef8e09c57
  DOWNLOAD_ONLY YES
)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/dbc_parser.cmake)
pj_add_dbc_parser_library(dbc ${dbc_parser_cpp_SOURCE_DIR} ${fast_float_SOURCE_DIR})

add_library(pj_can_dbc STATIC can_decoder.cpp can_topic.cpp dictionary_paths.cpp signal_row.cpp)
# Static lib goes inside SHARED plugin .so files -> must be position-independent.
set_target_properties(pj_can_dbc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_can_dbc PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_can_dbc PRIVATE ${PJ_WARNING_FLAGS})
endif()
target_include_directories(pj_can_dbc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# dbc is PRIVATE: can_decoder.hpp hides it behind a pImpl (no dbc header leaks).
target_link_libraries(pj_can_dbc PUBLIC plotjuggler_sdk::base PRIVATE dbc)

# GTest-guarded so this common target can be configured unconditionally.
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_can_dbc_test tests/can_decoder_test.cpp tests/dictionary_paths_test.cpp)
  target_compile_features(pj_can_dbc_test PRIVATE cxx_std_20)
  # field_test_helpers.hpp: shared findField() test helper, also used by
  # data_load_blf/data_load_candump's tests.
  target_include_directories(pj_can_dbc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_can_dbc_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_can_dbc_test PRIVATE pj_can_dbc GTest::gtest_main)
  add_test(NAME pj_can_dbc_test COMMAND pj_can_dbc_test)
endif()
