# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MIT

find_package(lz4 REQUIRED)
find_package(zstd REQUIRED)

# Conan hides headers for dependencies reached only through Arrow. Imported
# targets can therefore exist with empty include directories, silently falling
# back to host headers. Every consumer recipe must list these direct requires.
function(pj_mosaico_descriptor_import_require_visible_headers target)
  get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
  string(REGEX REPLACE "\\$<\\$<CONFIG:[^>]*>:([^>]*)>" "\\1" dirs "${dirs}")
  string(REPLACE ";" "" dirs "${dirs}")
  if("${dirs}" STREQUAL "" OR "${dirs}" MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "pj_mosaico_descriptor_import: ${target} exposes no include directories. "
      "Every consumer recipe must list lz4 and zstd as direct requires "
      "so the artifact implementation cannot compile against host headers.")
  endif()
endfunction()
pj_mosaico_descriptor_import_require_visible_headers(LZ4::lz4_static)
pj_mosaico_descriptor_import_require_visible_headers(zstd::libzstd_static)

add_library(pj_mosaico_descriptor_import STATIC
  arrow_cache_artifact.cpp
  artifact_capture.cpp
  artifact_replay.cpp
  source_descriptor.cpp
)

set_target_properties(pj_mosaico_descriptor_import PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_mosaico_descriptor_import PUBLIC cxx_std_20)
target_include_directories(pj_mosaico_descriptor_import PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# The repository's single vendored header-only MCAP copy. arrow_cache_artifact
# is this target's one MCAP_IMPLEMENTATION translation unit.
target_include_directories(pj_mosaico_descriptor_import SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../data_load_mcap/contrib
)
target_link_libraries(pj_mosaico_descriptor_import PUBLIC
  plotjuggler_sdk::descriptor_import_support
  LZ4::lz4_static
  zstd::libzstd_static
  ${PJ_MOSAICO_CONNECTOR_ARROW_TARGET}
)

if(MSVC)
  target_compile_options(pj_mosaico_descriptor_import PRIVATE /bigobj)
else()
  target_compile_options(pj_mosaico_descriptor_import PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
endif()

if(BUILD_TESTING AND TARGET GTest::gtest_main)
  set(PJ_MOSAICO_DESCRIPTOR_IMPORT_TEST_WARNINGS "")
  if(NOT MSVC)
    set(PJ_MOSAICO_DESCRIPTOR_IMPORT_TEST_WARNINGS
      -Wall -Wextra -Werror
      -Wno-conversion -Wno-unused-parameter -Wno-unused-function)
  endif()

  set(PJ_MOSAICO_DESCRIPTOR_IMPORT_TESTS
    source_descriptor_test
    arrow_cache_artifact_test
    artifact_replay_test
    artifact_capture_test
    cache_maintenance_test
    capture_strategy_test
  )
  foreach(_test ${PJ_MOSAICO_DESCRIPTOR_IMPORT_TESTS})
    add_executable(pj_mosaico_descriptor_import_${_test} tests/${_test}.cpp)
    target_link_libraries(pj_mosaico_descriptor_import_${_test} PRIVATE
      pj_mosaico_descriptor_import
      GTest::gtest_main
    )
    target_compile_features(pj_mosaico_descriptor_import_${_test} PRIVATE cxx_std_20)
    target_compile_options(
      pj_mosaico_descriptor_import_${_test} PRIVATE ${PJ_MOSAICO_DESCRIPTOR_IMPORT_TEST_WARNINGS})
    if(MSVC)
      target_compile_options(pj_mosaico_descriptor_import_${_test} PRIVATE /bigobj)
    endif()
    add_test(NAME MosaicoDescriptorImport_${_test} COMMAND pj_mosaico_descriptor_import_${_test})
  endforeach()

  target_compile_definitions(pj_mosaico_descriptor_import_source_descriptor_test PRIVATE
    MOSAICO_VECTORS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/source-descriptor-vectors.json"
  )
endif()
