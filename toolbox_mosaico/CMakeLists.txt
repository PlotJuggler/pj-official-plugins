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
# SDK does not find_package() them. Pin the source to a release tag: the
# downstream cancellation patch below is deliberately tied to that snapshot and
# must fail loudly if its context stops matching, rather than silently building
# without active stream cancellation.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
set(MOSAICO_SDK_VERSION v0.1.0)
CPMAddPackage(
  NAME mosaico_sdk
  URL https://github.com/PlotJuggler/mosaico-cpp-sdk/archive/refs/tags/${MOSAICO_SDK_VERSION}.zip
  PATCHES ${CMAKE_CURRENT_LIST_DIR}/patches/mosaico_sdk_active_pull_cancel.patch
)

set(MOSAICO_SOURCES
  src/toolbox_mosaico.cpp
  src/mosaico_dialog.cpp
  src/fetch_worker.cpp
  src/arrow_ipc_message.cpp
  src/settings_store.cpp
  src/server_history.cpp
  src/source_descriptor.cpp
  src/credential_resolve.cpp
  src/descriptor_import_provider.cpp
)

# Arrow/Flight translation units instantiate enough templates to exceed
# the COFF section limit without /bigobj (C1128). Apply it directory-wide on MSVC
# so the plugin and every Arrow-linked test target below inherit it.
if(MSVC)
  add_compile_options(/bigobj)
endif()

add_library(toolbox_mosaico_plugin SHARED ${MOSAICO_SOURCES})

target_compile_features(toolbox_mosaico_plugin PRIVATE cxx_std_20)
# Don't apply ${PJ_WARNING_FLAGS} here — sol2 triggers -Wconversion and
# -Wunused-parameter under -Werror. The core dialog/state-machine code is
# reviewed manually. These GNU flags are not valid for cl, so gate them to
# non-MSVC compilers.
if(NOT MSVC)
  target_compile_options(toolbox_mosaico_plugin PRIVATE
    -Wall -Wextra -Werror
    -Wno-conversion -Wno-unused-parameter -Wno-unused-function
  )
endif()

target_include_directories(toolbox_mosaico_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(toolbox_mosaico_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::source
  fmt::fmt
  nlohmann_json::nlohmann_json
  mosaico_sdk
  pj_common_query
  pj_transfer_rate
  lua::lua
  sol2::sol2
)

# gRPC++ is optional at find-package time; link only if probed.
if(MOSAICO_GRPCPP_TARGET)
  target_link_libraries(toolbox_mosaico_plugin PRIVATE ${MOSAICO_GRPCPP_TARGET})
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(toolbox_mosaico_plugin
  REQUIRED_EXPORTS PJ_get_toolbox_vtable PJ_get_dialog_vtable)

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

  # The query tests below relax warnings with GNU flags (sol2 trips
  # -Wconversion / -Wunused-parameter under -Werror). MSVC rejects those
  # (e.g. /Wextra is a fatal command-line error D8021), so map the set to
  # nothing on MSVC and to the GNU flags elsewhere.
  if(MSVC)
    set(MOSAICO_QUERY_TEST_WARNINGS "")
  else()
    set(MOSAICO_QUERY_TEST_WARNINGS
      -Wall -Wextra -Werror
      -Wno-conversion -Wno-unused-parameter -Wno-unused-function)
  endif()

  # arrow-ipc framing (timestamp detection, host timestamp, IPC round trip).
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

  add_executable(toolbox_mosaico_arrow_ipc_message_test
    tests/arrow_ipc_message_test.cpp
    src/arrow_ipc_message.cpp
  )
  target_link_libraries(toolbox_mosaico_arrow_ipc_message_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    ${_test_arrow_target}
    nlohmann_json::nlohmann_json
    fmt::fmt
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_arrow_ipc_message_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoArrowIpcMessageTest COMMAND toolbox_mosaico_arrow_ipc_message_test)

  # StoppableThread is pure-std with no Arrow/Flight surface, so its test links
  # nothing but gtest.
  add_executable(toolbox_mosaico_stoppable_thread_test tests/stoppable_thread_test.cpp)
  target_link_libraries(toolbox_mosaico_stoppable_thread_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_stoppable_thread_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_stoppable_thread_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_stoppable_thread_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  add_test(NAME ToolboxMosaicoStoppableThreadTest COMMAND toolbox_mosaico_stoppable_thread_test)

  add_executable(toolbox_mosaico_ingest_progress_test
    tests/ingest_progress_test.cpp
    src/fetch_worker.cpp
    src/arrow_ipc_message.cpp
    src/source_descriptor.cpp
  )
  target_link_libraries(toolbox_mosaico_ingest_progress_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    plotjuggler_sdk::source
    mosaico_sdk
    fmt::fmt
    nlohmann_json::nlohmann_json
    ${_test_arrow_target}
    GTest::gtest_main
  )
  if(MOSAICO_GRPCPP_TARGET)
    target_link_libraries(toolbox_mosaico_ingest_progress_test PRIVATE ${MOSAICO_GRPCPP_TARGET})
  endif()
  target_include_directories(toolbox_mosaico_ingest_progress_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_ingest_progress_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_ingest_progress_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  add_test(NAME ToolboxMosaicoIngestProgressTest COMMAND toolbox_mosaico_ingest_progress_test)

  # Ontology routing (image vs point-cloud/scalar). Compiles only the SDK's
  # light metadata.cpp (arrow + nlohmann) — no Flight/gRPC — like the
  # detect-timestamp test.
  add_executable(toolbox_mosaico_ontology_routing_test
    tests/ontology_routing_test.cpp
    ${mosaico_sdk_SOURCE_DIR}/src/flight/metadata.cpp
  )
  target_link_libraries(toolbox_mosaico_ontology_routing_test PRIVATE
    plotjuggler_sdk::base
    ${_test_arrow_target}
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_ontology_routing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${mosaico_sdk_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoOntologyRoutingTest COMMAND toolbox_mosaico_ontology_routing_test)

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

  # Lua-backed query Engine (eval/validate/metadata globals). The query
  # LANGUAGE tests live with common/query. Needs lua + sol2 only.
  add_executable(toolbox_mosaico_query_engine_test
    tests/query_engine_test.cpp
  )
  target_link_libraries(toolbox_mosaico_query_engine_test PRIVATE
    pj_common_query lua::lua sol2::sol2 GTest::gtest_main
  )
  target_compile_features(toolbox_mosaico_query_engine_test PRIVATE cxx_std_20)
  # Match the plugin's relaxed flags: sol2 trips -Wconversion /
  # -Wunused-parameter under -Werror.
  target_compile_options(toolbox_mosaico_query_engine_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  if(MSVC)
    # This test embeds raw string literals inside gtest EXPECT_EQ() macros. MSVC's
    # traditional preprocessor mis-tokenizes raw strings in macro arguments
    # (C2017 illegal escape sequence / C3688 bad literal suffix), so build it with
    # the conforming preprocessor.
    target_compile_options(toolbox_mosaico_query_engine_test PRIVATE /Zc:preprocessor)
  endif()
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
    pj_common_query lua::lua sol2::sol2 GTest::gtest_main
  )
  target_compile_features(toolbox_mosaico_query_filter_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_query_filter_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  target_include_directories(toolbox_mosaico_query_filter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )
  add_test(NAME ToolboxMosaicoQueryFilterTest COMMAND toolbox_mosaico_query_filter_test)

  # Assist model: the Key/Op/Value dropdown + PLUS state machine for the metadata
  # filter, encoding the query-assist contract (replace-at-caret vs stage-then-PLUS).
  # Pure header logic (query_assist.h over pj_common_query) — no Lua, no Arrow/Flight.
  add_executable(toolbox_mosaico_assist_test
    tests/assist_test.cpp
  )
  target_link_libraries(toolbox_mosaico_assist_test PRIVATE pj_common_query GTest::gtest_main)
  target_compile_features(toolbox_mosaico_assist_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_assist_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  target_include_directories(toolbox_mosaico_assist_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )
  add_test(NAME ToolboxMosaicoAssistTest COMMAND toolbox_mosaico_assist_test)

  # Column-sort permutation (numeric vs lexicographic, stability). Header-only.
  add_executable(toolbox_mosaico_table_sort_test
    tests/table_sort_test.cpp
  )
  target_link_libraries(toolbox_mosaico_table_sort_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_table_sort_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoTableSortTest COMMAND toolbox_mosaico_table_sort_test)

  # Filter-exempt selection merge (text-keyed picker contract). Header-only.
  add_executable(toolbox_mosaico_selection_merge_test
    tests/selection_merge_test.cpp
  )
  target_link_libraries(toolbox_mosaico_selection_merge_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_mosaico_selection_merge_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME ToolboxMosaicoSelectionMergeTest COMMAND toolbox_mosaico_selection_merge_test)

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

  add_executable(toolbox_mosaico_settings_store_test
    tests/settings_store_test.cpp
    src/settings_store.cpp
  )
  target_include_directories(toolbox_mosaico_settings_store_test PRIVATE src)
  target_link_libraries(toolbox_mosaico_settings_store_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  add_test(NAME toolbox_mosaico_settings_store_test COMMAND toolbox_mosaico_settings_store_test)

  # Source descriptor: typed parse/validate, canonical bytes + identity
  # conformance against docs/source-descriptor-vectors.json, rejection
  # matrix. nlohmann + the SDK descriptor_import helpers only.
  add_executable(toolbox_mosaico_source_descriptor_test
    tests/source_descriptor_test.cpp
    src/source_descriptor.cpp
  )
  target_compile_definitions(toolbox_mosaico_source_descriptor_test PRIVATE
    MOSAICO_VECTORS_JSON="${CMAKE_CURRENT_SOURCE_DIR}/docs/source-descriptor-vectors.json"
  )
  target_link_libraries(toolbox_mosaico_source_descriptor_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    plotjuggler_sdk::source nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_source_descriptor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_source_descriptor_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_source_descriptor_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  add_test(NAME ToolboxMosaicoSourceDescriptorTest COMMAND toolbox_mosaico_source_descriptor_test)

  # Headless credential origin guard + interactive resolution.
  add_executable(toolbox_mosaico_credential_resolve_test
    tests/credential_resolve_test.cpp
    src/credential_resolve.cpp
    src/settings_store.cpp
    src/server_history.cpp
  )
  target_link_libraries(toolbox_mosaico_credential_resolve_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    plotjuggler_sdk::source nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_mosaico_credential_resolve_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_credential_resolve_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_credential_resolve_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  add_test(NAME ToolboxMosaicoCredentialResolveTest COMMAND toolbox_mosaico_credential_resolve_test)

  # pj.descriptor_import.v1 provider: query taxonomy + startImport
  # fail-closed rejections + watchdog inertness. Pulls in the fetch worker,
  # so it links the Mosaico SDK / Arrow / gRPC like the ingest-progress test.
  add_executable(toolbox_mosaico_descriptor_import_contract_test
    tests/descriptor_import_contract_test.cpp
    src/descriptor_import_provider.cpp
    src/fetch_worker.cpp
    src/arrow_ipc_message.cpp
    src/credential_resolve.cpp
    src/source_descriptor.cpp
    src/settings_store.cpp
    src/server_history.cpp
  )
  target_link_libraries(toolbox_mosaico_descriptor_import_contract_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    plotjuggler_sdk::source
    mosaico_sdk
    fmt::fmt
    nlohmann_json::nlohmann_json
    ${_test_arrow_target}
    GTest::gtest_main
  )
  if(MOSAICO_GRPCPP_TARGET)
    target_link_libraries(toolbox_mosaico_descriptor_import_contract_test PRIVATE ${MOSAICO_GRPCPP_TARGET})
  endif()
  target_include_directories(toolbox_mosaico_descriptor_import_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_descriptor_import_contract_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_descriptor_import_contract_test PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
  add_test(NAME ToolboxMosaicoDescriptorImportContractTest COMMAND toolbox_mosaico_descriptor_import_contract_test)

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
    plotjuggler_sdk::plugin_sdk  # PJ::sdk::getEnv (header-only, portable env read)
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
    plotjuggler_sdk::plugin_sdk  # PJ::sdk::getEnv (header-only, portable env read)
  )
  target_include_directories(toolbox_mosaico_live_harness PRIVATE
    ${mosaico_sdk_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_mosaico_live_harness PRIVATE cxx_std_20)
  target_compile_options(toolbox_mosaico_live_harness PRIVATE ${MOSAICO_QUERY_TEST_WARNINGS})
endif()
