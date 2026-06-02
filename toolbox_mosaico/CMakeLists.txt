find_package(nlohmann_json REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(fmt REQUIRED)
find_package(lua REQUIRED)
find_package(sol2 REQUIRED)

# Probe for a real Arrow Flight target (the candidate set must match the SDK's
# in mosaico-cpp-sdk). Crucially this does NOT fall back to plain arrow::arrow:
# Conan's arrow only carries Flight where arrow:with_flight_rpc actually built
# it (Linux), and on Windows/macOS arrow::arrow exists WITHOUT Flight. Treating
# arrow::arrow as Flight there let the plugin proceed into the SDK, which then
# FATAL_ERROR'd on the missing Flight target. With a strict probe, Flight-less
# platforms fall through to the skip below — Mosaico is Linux-only for Flight,
# same as PJ3 — instead of failing the whole build.
set(MOSAICO_ARROW_FLIGHT_TARGET "")
foreach(_candidate
        ArrowFlight::arrow_flight_static
        arrow::arrow_flight
        ArrowFlight::arrow_flight
        arrow::arrow_flight_static)
  if(TARGET ${_candidate})
    set(MOSAICO_ARROW_FLIGHT_TARGET ${_candidate})
    break()
  endif()
endforeach()

if(NOT MOSAICO_ARROW_FLIGHT_TARGET)
  message(STATUS "[toolbox_mosaico] Arrow Flight target not found — skipping plugin (Flight unavailable on this platform).")
  return()
endif()

# gRPC++ — required because static Arrow Flight does not propagate gRPC
# symbols. Same probe as PJ3.
set(MOSAICO_GRPCPP_TARGET "")
find_package(gRPC QUIET CONFIG)
if(TARGET gRPC::grpc++)
  set(MOSAICO_GRPCPP_TARGET gRPC::grpc++)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MOSAICO_GRPCPP QUIET IMPORTED_TARGET grpc++)
    if(TARGET PkgConfig::MOSAICO_GRPCPP)
      set(MOSAICO_GRPCPP_TARGET PkgConfig::MOSAICO_GRPCPP)
    endif()
  endif()
endif()

# Conan's CMakeDeps creates lowercase arrow::arrow; the vendored mosaico_sdk
# CMakeLists references it directly.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# The Mosaico client SDK lives in its own repo and is fetched via CPM. Arrow
# (+ Flight) and nlohmann_json targets must already exist (created above) — the
# SDK does not find_package() them. CPM add_subdirectory's the fetched tree, so
# it defines the `mosaico_sdk` target exactly as the old vendored copy did.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
CPMAddPackage(
  NAME mosaico_sdk
  URL https://github.com/PlotJuggler/mosaico-cpp-sdk/archive/refs/heads/development.zip
)

set(MOSAICO_SOURCES
  src/toolbox_mosaico.cpp
  src/mosaico_dialog.cpp
  src/fetch_worker.cpp
  src/arrow_ingest.cpp
  src/settings_store.cpp
  src/server_history.cpp
  src/core/time_format.cpp
)

add_library(toolbox_mosaico_plugin SHARED ${MOSAICO_SOURCES})

target_compile_features(toolbox_mosaico_plugin PRIVATE cxx_std_20)
# Don't apply ${PJ_WARNING_FLAGS} here — the vendored PJ3 query/* headers
# and sol2 trigger -Wconversion and -Wunused-parameter under -Werror. The
# core dialog/state-machine code is reviewed manually; this matches PJ3's
# own build flags for its query/ subdir.
target_compile_options(toolbox_mosaico_plugin PRIVATE
  -Wall -Wextra -Werror
  -Wno-conversion -Wno-unused-parameter -Wno-unused-function
)

target_include_directories(toolbox_mosaico_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/query
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(toolbox_mosaico_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  fmt::fmt
  nlohmann_json::nlohmann_json
  mosaico_sdk
  lua::lua
  sol2::sol2
)

# gRPC++ is optional at find-package time; link only if probed.
if(MOSAICO_GRPCPP_TARGET)
  target_link_libraries(toolbox_mosaico_plugin PRIVATE ${MOSAICO_GRPCPP_TARGET})
endif()

pj_embed_ui(toolbox_mosaico_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/mosaico_panel.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mosaico_panel_ui.hpp
  VAR_NAME kMosaicoPanelUi
)

pj_embed_ui(toolbox_mosaico_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/cert_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/cert_dialog_ui.hpp
  VAR_NAME kCertDialogUi
)

pj_embed_manifest(toolbox_mosaico_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mosaico_panel_manifest.hpp
  VAR_NAME kMosaicoPanelManifest
)

pj_emit_plugin_manifest(toolbox_mosaico_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# ----------------------------------------------------------------------------
# Tests — independent of Mosaico SDK / Flight / gRPC.
# ----------------------------------------------------------------------------
if(BUILD_TESTING)
  find_package(GTest REQUIRED)

  # detectTimestampColumn over canned ArrowSchema fixtures.
  # Re-resolves the Arrow target name (Conan: arrow::arrow, upstream:
  # Arrow::arrow_static) since the cached _mosaico_arrow_target variable
  # may not be visible across the BUILD_TESTING gate.
  set(_test_arrow_target "")
  foreach(_candidate arrow::arrow Arrow::arrow_static Arrow::arrow_shared Arrow::arrow)
    if(TARGET ${_candidate})
      set(_test_arrow_target ${_candidate})
      break()
    endif()
  endforeach()

  add_executable(toolbox_mosaico_detect_timestamp_test
    tests/detect_timestamp_test.cpp
    src/arrow_ingest.cpp
  )
  target_link_libraries(toolbox_mosaico_detect_timestamp_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    ${_test_arrow_target}
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_detect_timestamp_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoDetectTimestampTest COMMAND toolbox_mosaico_detect_timestamp_test)

  # Image-topic metadata builder: pure string logic, header-only, so this needs
  # nothing but GTest. Pins the cross-repo metadata_json contract.
  add_executable(toolbox_mosaico_image_metadata_test
    tests/image_metadata_test.cpp
  )
  target_link_libraries(toolbox_mosaico_image_metadata_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_image_metadata_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoImageMetadataTest COMMAND toolbox_mosaico_image_metadata_test)

  # Ontology routing (image vs point-cloud/scalar). Compiles only the SDK's
  # light metadata.cpp (arrow + nlohmann) — no Flight/gRPC — like the
  # detect-timestamp test.
  add_executable(toolbox_mosaico_ontology_routing_test
    tests/ontology_routing_test.cpp
    ${mosaico_sdk_SOURCE_DIR}/src/flight/metadata.cpp
  )
  target_link_libraries(toolbox_mosaico_ontology_routing_test PRIVATE
    ${_test_arrow_target}
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_ontology_routing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${mosaico_sdk_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoOntologyRoutingTest COMMAND toolbox_mosaico_ontology_routing_test)

  # Multi-topic grouping contract: one Download (sequence) → ONE dataset, with
  # topics registered under their BARE names. Drives the real arrow_ingest
  # helper against a recording fake toolbox host (no Flight/gRPC).
  add_executable(toolbox_mosaico_fetch_grouping_test
    tests/fetch_grouping_test.cpp
    src/arrow_ingest.cpp
  )
  target_link_libraries(toolbox_mosaico_fetch_grouping_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    ${_test_arrow_target}
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_fetch_grouping_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoFetchGroupingTest COMMAND toolbox_mosaico_fetch_grouping_test)

  # Per-frame image serialization: each Arrow row -> one canonical PJ.Image blob
  # (pj_base codec), driven against a recording fake toolbox host. Builds tables
  # with the real server's BINARY_VIEW/STRING_VIEW column types (no Flight/gRPC).
  add_executable(toolbox_mosaico_image_serialize_test
    tests/image_serialize_test.cpp
    src/arrow_ingest.cpp
  )
  target_link_libraries(toolbox_mosaico_image_serialize_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    ${_test_arrow_target}
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_image_serialize_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoImageSerializeTest COMMAND toolbox_mosaico_image_serialize_test)

  # Real-server regression: replays the captured jai/rgb Arrow batch (PNG-wrapped
  # Bayer, BINARY_VIEW/STRING_VIEW columns) through the real producer and checks
  # every blob round-trips to a valid per-frame PJ::Image. SKIPS if the (~31 MB,
  # uncommitted) fixture is absent; regenerate it via toolbox_mosaico_live_harness.
  add_executable(toolbox_mosaico_image_real_fixture_test
    tests/image_real_fixture_test.cpp
    src/arrow_ingest.cpp
  )
  target_link_libraries(toolbox_mosaico_image_real_fixture_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    ${_test_arrow_target}
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_image_real_fixture_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoImageRealFixtureTest COMMAND toolbox_mosaico_image_real_fixture_test)

  # Fetch-completion policy (close/import/error-dedup). Header-only, std only.
  add_executable(toolbox_mosaico_fetch_summary_test
    tests/fetch_summary_test.cpp
  )
  target_link_libraries(toolbox_mosaico_fetch_summary_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_fetch_summary_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoFetchSummaryTest COMMAND toolbox_mosaico_fetch_summary_test)

  # SequencePicker date/time filter incl. "Every day". Header-only, std only.
  add_executable(toolbox_mosaico_date_filter_test
    tests/date_filter_test.cpp
  )
  target_link_libraries(toolbox_mosaico_date_filter_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_date_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoDateFilterTest COMMAND toolbox_mosaico_date_filter_test)

  # Name filter (substring + ".*" regex).
  add_executable(toolbox_mosaico_name_filter_test
    tests/name_filter_test.cpp
  )
  target_link_libraries(toolbox_mosaico_name_filter_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_name_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoNameFilterTest COMMAND toolbox_mosaico_name_filter_test)

  # Query engine (Lexer/Parser/serialize/Query/complete/analyze/Engine). The
  # query/* headers are byte-for-byte the PJ3 engine; these tests pin their
  # behavior. Lua-backed eval/validate needs lua + sol2 only.
  add_executable(toolbox_mosaico_query_engine_test
    tests/query_engine_test.cpp
  )
  target_link_libraries(toolbox_mosaico_query_engine_test PRIVATE
    lua::lua sol2::sol2 GTest::gtest_main
  )
  target_compile_features(toolbox_mosaico_query_engine_test PRIVATE cxx_std_20)
  # Match the plugin's relaxed flags: the vendored PJ3 query/* headers and sol2
  # trip -Wconversion / -Wunused-parameter under -Werror.
  target_compile_options(toolbox_mosaico_query_engine_test PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
  target_include_directories(toolbox_mosaico_query_engine_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )
  add_test(NAME ToolboxMosaicoQueryEngineTest COMMAND toolbox_mosaico_query_engine_test)

  # Query-filter / visible-row helper: validity-gating + name/date combine.
  # Header-only logic plus the Lua engine; needs lua + sol2 but no Arrow/Flight.
  add_executable(toolbox_mosaico_query_filter_test
    tests/query_filter_test.cpp
  )
  target_link_libraries(toolbox_mosaico_query_filter_test PRIVATE
    lua::lua sol2::sol2 GTest::gtest_main
  )
  target_compile_features(toolbox_mosaico_query_filter_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_query_filter_test PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
  target_include_directories(toolbox_mosaico_query_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )
  add_test(NAME ToolboxMosaicoQueryFilterTest COMMAND toolbox_mosaico_query_filter_test)

  # Column-sort permutation (numeric vs lexicographic, stability). Header-only.
  add_executable(toolbox_mosaico_table_sort_test
    tests/table_sort_test.cpp
  )
  target_link_libraries(toolbox_mosaico_table_sort_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_table_sort_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoTableSortTest COMMAND toolbox_mosaico_table_sort_test)

  # Human-readable byte formatting (1024-based, PJ3 parity). Header-only.
  add_executable(toolbox_mosaico_format_utils_test
    tests/format_utils_test.cpp
  )
  target_link_libraries(toolbox_mosaico_format_utils_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_format_utils_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoFormatUtilsTest COMMAND toolbox_mosaico_format_utils_test)

  # PJ3-ported leaf utility tests. Standard library only — no Arrow Flight,
  # no toolbox host.
  add_executable(toolbox_mosaico_server_history_test
    tests/server_history_test.cpp
    src/server_history.cpp
  )
  target_link_libraries(toolbox_mosaico_server_history_test PRIVATE
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_server_history_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoServerHistoryTest COMMAND toolbox_mosaico_server_history_test)

  add_executable(toolbox_mosaico_tls_utils_test
    tests/tls_utils_test.cpp
  )
  target_link_libraries(toolbox_mosaico_tls_utils_test PRIVATE
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_tls_utils_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoTlsUtilsTest COMMAND toolbox_mosaico_tls_utils_test)

  add_executable(toolbox_mosaico_time_format_test
    tests/time_format_test.cpp
    src/core/time_format.cpp
  )
  target_link_libraries(toolbox_mosaico_time_format_test PRIVATE
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_time_format_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoTimeFormatTest COMMAND toolbox_mosaico_time_format_test)

  add_executable(toolbox_mosaico_settings_store_test
    tests/settings_store_test.cpp
    src/settings_store.cpp
  )
  target_include_directories(toolbox_mosaico_settings_store_test PRIVATE src)
  target_link_libraries(toolbox_mosaico_settings_store_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  add_test(NAME toolbox_mosaico_settings_store_test COMMAND toolbox_mosaico_settings_store_test)

  # Live-server smoke: connects to demo.mosaico.dev, looks for an
  # image-ontology topic, pulls a 1-second window. Opt-in: only runs
  # when MOSAICO_LIVE_SMOKE=1 is set in the test env (skipped otherwise).
  add_executable(toolbox_mosaico_image_download_smoke
    tests/image_download_smoke.cpp
  )
  target_link_libraries(toolbox_mosaico_image_download_smoke PRIVATE
    mosaico_sdk
    ${_test_arrow_target}
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_image_download_smoke PRIVATE
    ${mosaico_sdk_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoImageDownloadSmoke COMMAND toolbox_mosaico_image_download_smoke)
  set_tests_properties(ToolboxMosaicoImageDownloadSmoke PROPERTIES
    ENVIRONMENT "MOSAICO_LIVE_SMOKE=1"
    TIMEOUT 60
  )

  # Live-server diagnostic harness (Task 0.1). Standalone executable
  # that connects to demo.mosaico.dev, discovers an RGB image topic, pulls the
  # first record batch, prints the COMPLETE Arrow schema + ROW-0 image metadata,
  # and writes the batch to tests/fixtures/<slug>_batch.arrow (Arrow IPC file).
  # Not registered with ctest — it is a hand-run diagnostic that requires
  # MOSAICO_API_KEY in the environment (prints "SKIP: no MOSAICO_API_KEY" and
  # exits 0 when absent). Links the SDK + Arrow only; no GTest.
  add_executable(toolbox_mosaico_live_harness
    tests/live_server_harness.cpp
  )
  target_link_libraries(toolbox_mosaico_live_harness PRIVATE
    mosaico_sdk
    ${_test_arrow_target}
  )
  target_include_directories(toolbox_mosaico_live_harness PRIVATE
    ${mosaico_sdk_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_live_harness PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_live_harness PRIVATE -Wall -Wextra)
endif()
