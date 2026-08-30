# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MIT

add_library(pj_mosaico_ingest STATIC
  arrow_ingest.cpp
  arrow_ingest.hpp
  image_metadata.hpp
  object_metadata.hpp
)

set_target_properties(pj_mosaico_ingest PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_mosaico_ingest PUBLIC cxx_std_20)
target_include_directories(pj_mosaico_ingest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pj_mosaico_ingest PUBLIC
  plotjuggler_sdk::plugin_sdk
  ${PJ_MOSAICO_CONNECTOR_ARROW_TARGET}
)

# Arrow-compute instantiations exceed the COFF section limit without /bigobj.
if(MSVC)
  target_compile_options(pj_mosaico_ingest PRIVATE /bigobj)
else()
  target_compile_options(pj_mosaico_ingest PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
endif()

if(BUILD_TESTING AND TARGET GTest::gtest_main)
  set(PJ_MOSAICO_INGEST_TEST_WARNINGS "")
  if(NOT MSVC)
    set(PJ_MOSAICO_INGEST_TEST_WARNINGS
      -Wall -Wextra -Werror
      -Wno-conversion -Wno-unused-parameter -Wno-unused-function)
  endif()

  set(PJ_MOSAICO_INGEST_TESTS
    detect_timestamp_test
    fetch_grouping_test
    image_serialize_test
    object_serialize_test
    image_real_fixture_test
    image_metadata_test
  )
  foreach(_test ${PJ_MOSAICO_INGEST_TESTS})
    add_executable(pj_mosaico_ingest_${_test} tests/${_test}.cpp)
    target_link_libraries(pj_mosaico_ingest_${_test} PRIVATE
      pj_mosaico_ingest
      GTest::gtest_main
    )
    target_compile_features(pj_mosaico_ingest_${_test} PRIVATE cxx_std_20)
    target_compile_options(pj_mosaico_ingest_${_test} PRIVATE ${PJ_MOSAICO_INGEST_TEST_WARNINGS})
    if(MSVC)
      target_compile_options(pj_mosaico_ingest_${_test} PRIVATE /bigobj)
    endif()
    add_test(NAME MosaicoIngest_${_test} COMMAND pj_mosaico_ingest_${_test})
  endforeach()
endif()
