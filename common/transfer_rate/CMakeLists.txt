# pj_transfer_rate — the rolling transfer-rate display helper shared by the
# cloud-browser dialogs (a monotonic five-second cumulative-byte window).
# Deliberately NOT SDK surface: a pure UI readout with no host contract.

add_library(pj_transfer_rate INTERFACE)
target_include_directories(pj_transfer_rate INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_transfer_rate INTERFACE cxx_std_20)

find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_transfer_rate_test tests/transfer_rate_test.cpp)
  target_compile_features(pj_transfer_rate_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_transfer_rate_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_transfer_rate_test PRIVATE pj_transfer_rate GTest::gtest_main)
  add_test(NAME pj_transfer_rate_test COMMAND pj_transfer_rate_test)
endif()
