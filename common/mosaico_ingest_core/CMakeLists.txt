# Mosaico ingest core, shared by toolbox_mosaico and data_load_mosaico_cache:
# the Arrow -> datastore ingest routines (column routing, canonical-object
# serialization, bulk columnar writes) and the layout re-import foundation
# (canonical source descriptor + vendored sha256, the request-addressed
# session file cache, and the Arrow-in-MCAP cache artifact format). SDK-free
# on purpose: plain Arrow + the plugin SDK only — NO Mosaico Flight SDK — so
# the cache loader replays a fetch on platforms where Flight never builds.
# Internal target — not exported, not installed.

# Skipped gracefully when Arrow is unavailable (same contract as
# common/arrow_helpers): real consumers do their own find_package and error
# there, never silently run without the target.
set(MOSAICO_INGEST_ARROW_TARGET "")
foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
  if(TARGET ${_candidate})
    set(MOSAICO_INGEST_ARROW_TARGET ${_candidate})
    break()
  endif()
endforeach()
if(NOT MOSAICO_INGEST_ARROW_TARGET)
  find_package(Arrow QUIET CONFIG)
  foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
    if(TARGET ${_candidate})
      set(MOSAICO_INGEST_ARROW_TARGET ${_candidate})
      break()
    endif()
  endforeach()
endif()
if(NOT MOSAICO_INGEST_ARROW_TARGET)
  message(STATUS "mosaico_ingest_core: Arrow not found, skipping")
  return()
endif()

find_package(nlohmann_json REQUIRED)
# The cache artifact is an MCAP container (vendored header-only mcap from
# data_load_mcap/contrib); lz4 + zstd come from conan for chunk compression.
find_package(lz4 REQUIRED)
find_package(zstd REQUIRED)

add_library(mosaico_ingest_core STATIC
  arrow_cache_artifact.cpp
  arrow_ingest.cpp
  artifact_replay.cpp
  cache_tee_session.cpp
  core/file_lock.cpp
  core/fs_durability.cpp
  core/origin_match.cpp
  core/sha256.cpp
  session_file_cache.cpp
  source_descriptor.cpp
  trusted_origins.cpp
)
# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(mosaico_ingest_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mosaico_ingest_core PUBLIC cxx_std_20)
target_include_directories(mosaico_ingest_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
# Vendored header-only mcap (the repo's single copy lives with data_load_mcap;
# arrow_cache_artifact.cpp is this library's one MCAP_IMPLEMENTATION TU — the
# consuming plugins' export allowlists keep those symbols DSO-private).
target_include_directories(mosaico_ingest_core SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../data_load_mcap/contrib
)
target_link_libraries(mosaico_ingest_core PUBLIC
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LZ4::lz4_static
  zstd::libzstd_static
  ${MOSAICO_INGEST_ARROW_TARGET}
)
# Arrow-compute instantiations exceed the COFF section limit without /bigobj.
if(MSVC)
  target_compile_options(mosaico_ingest_core PRIVATE /bigobj)
endif()
# The Arrow ingest translation units predate this repo's full warning set;
# match the relaxed flags they were written under (same set toolbox_mosaico
# uses; PJ_WARNING_FLAGS is deliberately not applied).
if(NOT MSVC)
  target_compile_options(mosaico_ingest_core PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
endif()

# ---------------------------------------------------------------------------
# Tests — everything here is Flight-free and runs on every platform.
# ---------------------------------------------------------------------------
if(BUILD_TESTING AND TARGET GTest::gtest_main)
  set(MOSAICO_INGEST_TEST_WARNINGS "")
  if(NOT MSVC)
    set(MOSAICO_INGEST_TEST_WARNINGS
      -Wall -Wextra -Werror
      -Wno-conversion -Wno-unused-parameter -Wno-unused-function)
  endif()

  # name = test source basename; every target links the core + gtest only.
  set(MOSAICO_INGEST_CORE_TESTS
    sha256_test
    origin_match_test
    trusted_origins_test
    source_descriptor_test
    session_file_cache_test
    arrow_cache_artifact_test
    artifact_replay_test
    cache_tee_session_test
    detect_timestamp_test
    fetch_grouping_test
    image_serialize_test
    object_serialize_test
    image_real_fixture_test
    image_metadata_test
  )
  foreach(_test ${MOSAICO_INGEST_CORE_TESTS})
    add_executable(mosaico_ingest_${_test} tests/${_test}.cpp)
    target_link_libraries(mosaico_ingest_${_test} PRIVATE
      mosaico_ingest_core
      GTest::gtest_main
    )
    target_compile_features(mosaico_ingest_${_test} PRIVATE cxx_std_20)
    target_compile_options(mosaico_ingest_${_test} PRIVATE ${MOSAICO_INGEST_TEST_WARNINGS})
    if(MSVC)
      target_compile_options(mosaico_ingest_${_test} PRIVATE /bigobj)
    endif()
    add_test(NAME MosaicoIngest_${_test} COMMAND mosaico_ingest_${_test})
  endforeach()

  # The canonicalization vectors are a contract file; every descriptor
  # consumer must reproduce those bytes verbatim.
  target_compile_definitions(mosaico_ingest_source_descriptor_test PRIVATE
    MOSAICO_VECTORS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/source-descriptor-vectors.json"
  )
endif()
