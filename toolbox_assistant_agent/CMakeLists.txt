find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

if(MSVC)
  # Conformant preprocessor for the SDK's variadic plugin macros; keep windows.h from defining
  # min/max. Directory scope: this plugin and its tests only.
  add_compile_options(/Zc:preprocessor)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Shared by the plugin and every test executable that links a real backend. Deliberately not an
# OBJECT library: the plugin compiles with ${PJ_WARNING_FLAGS} and the tests do not, so each
# target must compile its own copies under its own flags.
set(ASSISTANT_AGENT_COMMON_SOURCES
  src/harness_workdir.cpp
  src/features_digest.cpp
  src/claude_sessions.cpp
  src/codex_sessions.cpp
  src/codex_models.cpp
)

add_library(toolbox_assistant_agent_plugin SHARED
  src/assistant_toolbox.cpp
  src/assistant_dialog.cpp
  src/catalog_digest.cpp
  src/conversation_state.cpp
  src/settings_store.cpp
  src/claude_backend.cpp
  src/codex_backend.cpp
  ${ASSISTANT_AGENT_COMMON_SOURCES}
)

target_compile_features(toolbox_assistant_agent_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_assistant_agent_plugin PRIVATE ${PJ_WARNING_FLAGS})

# pj_derived_recipes: the catalog digest's dataset and object-topic listing.
target_link_libraries(toolbox_assistant_agent_plugin PRIVATE
  pj_derived_recipes
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

target_include_directories(toolbox_assistant_agent_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_assistant_agent_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/assistant_panel.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/assistant_panel_ui.hpp
  VAR_NAME kAssistantPanelUi
)

pj_embed_ui(toolbox_assistant_agent_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/assistant_settings.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/assistant_settings_ui.hpp
  VAR_NAME kAssistantSettingsUi
)

pj_embed_ui(toolbox_assistant_agent_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/rename_conversation.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/rename_conversation_ui.hpp
  VAR_NAME kRenameConversationUi
)

pj_embed_manifest(toolbox_assistant_agent_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/assistant_panel_manifest.hpp
  VAR_NAME kAssistantPanelManifest
)

pj_emit_plugin_manifest(toolbox_assistant_agent_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# Tests
if(TARGET GTest::gtest_main)
  add_executable(toolbox_assistant_agent_chat_session_test
    tests/chat_session_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_chat_session_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_chat_session_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_chat_session_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentChatSessionTest COMMAND toolbox_assistant_agent_chat_session_test)

  add_executable(toolbox_assistant_agent_usage_ledger_test
    tests/usage_ledger_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_usage_ledger_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_usage_ledger_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_usage_ledger_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentUsageLedgerTest COMMAND toolbox_assistant_agent_usage_ledger_test)

  add_executable(toolbox_assistant_agent_settings_store_test
    tests/settings_store_test.cpp
    src/settings_store.cpp
  )
  target_link_libraries(toolbox_assistant_agent_settings_store_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_settings_store_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_settings_store_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentSettingsStoreTest COMMAND toolbox_assistant_agent_settings_store_test)

  add_executable(toolbox_assistant_agent_conversation_state_test
    tests/conversation_state_test.cpp
    src/conversation_state.cpp
    src/settings_store.cpp
  )
  target_link_libraries(toolbox_assistant_agent_conversation_state_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_conversation_state_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_conversation_state_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentConversationStateTest COMMAND toolbox_assistant_agent_conversation_state_test)

  # Fixtures under tests/fixtures/sessions/ are copied to a temp dir per test; nothing reads or
  # writes the real ~/.claude store. plotjuggler_sdk::base is for PJ::sdk::getEnv.
  add_executable(toolbox_assistant_agent_claude_sessions_test
    tests/claude_sessions_test.cpp
    src/claude_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_claude_sessions_test PRIVATE
    plotjuggler_sdk::base pj_test_support nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_claude_sessions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_claude_sessions_test PRIVATE
    ASSISTANT_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sessions"
  )
  target_compile_features(toolbox_assistant_agent_claude_sessions_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentClaudeSessionsTest COMMAND toolbox_assistant_agent_claude_sessions_test)

  add_executable(toolbox_assistant_agent_model_picker_test
    tests/model_picker_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_model_picker_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_model_picker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_model_picker_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentModelPickerTest COMMAND toolbox_assistant_agent_model_picker_test)

  add_executable(toolbox_assistant_agent_catalog_digest_test
    tests/catalog_digest_test.cpp
    src/catalog_digest.cpp
  )
  target_link_libraries(toolbox_assistant_agent_catalog_digest_test PRIVATE
    pj_derived_recipes pj_test_support plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_catalog_digest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_catalog_digest_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCatalogDigestTest COMMAND toolbox_assistant_agent_catalog_digest_test)

  # Renders against the feature-catalog benchmark fixtures.
  add_executable(toolbox_assistant_agent_features_digest_test
    tests/features_digest_test.cpp
    src/features_digest.cpp
  )
  target_link_libraries(toolbox_assistant_agent_features_digest_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_features_digest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_features_digest_test PRIVATE
    ASSISTANT_FEATURES_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/benchmarks/features/fixtures"
  )
  target_compile_features(toolbox_assistant_agent_features_digest_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentFeaturesDigestTest COMMAND toolbox_assistant_agent_features_digest_test)

  add_executable(toolbox_assistant_agent_stream_json_test
    tests/stream_json_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_stream_json_test PRIVATE
    nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_stream_json_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_stream_json_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentStreamJsonTest COMMAND toolbox_assistant_agent_stream_json_test)

  # End to end against a REAL ClaudeBackend pointed at a throwaway HOME. No `claude` CLI is
  # spawned: a turn ends before the CLI lookup (no endpoint) or at it (a name that does not resolve).
  add_executable(toolbox_assistant_agent_assistant_dialog_test
    tests/assistant_dialog_test.cpp
    src/assistant_dialog.cpp
    src/catalog_digest.cpp
    src/claude_backend.cpp
    src/codex_backend.cpp
    src/conversation_state.cpp
    src/settings_store.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_assistant_dialog_test PRIVATE
    pj_derived_recipes pj_test_support plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_assistant_dialog_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  )
  target_compile_definitions(toolbox_assistant_agent_assistant_dialog_test PRIVATE
    ASSISTANT_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sessions"
    ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codex_sessions"
  )
  target_compile_features(toolbox_assistant_agent_assistant_dialog_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentAssistantDialogTest COMMAND toolbox_assistant_agent_assistant_dialog_test)

  # Standalone helper process spawned by subprocess_test.cpp and, on Windows (no shell-script CLI
  # stand-in), by writeFakeCliScript (tests/support/backend_test_helpers.hpp). Linked into no other
  # target, so it needs neither gtest nor the SDK. Declared before the targets that reference it
  # via ASSISTANT_CHILD_HELPER.
  add_executable(toolbox_assistant_agent_child_helper
    tests/support/child_helper.cpp
  )
  # Windows reuses platform_util.hpp's utf8ToWide/wideToUtf8; harmless elsewhere (header-only).
  target_include_directories(toolbox_assistant_agent_child_helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(toolbox_assistant_agent_child_helper PRIVATE cxx_std_20)

  # Live smoke test: spawns the real `claude` CLI against a running PlotJuggler's MCP server.
  # Opt-in (ASSISTANT_CLAUDE_SMOKE=1 plus ASSISTANT_MCP_URL/ASSISTANT_MCP_TOKEN), skipped otherwise
  # so CI stays offline. Also holds the offline fake-CLI tests, which need ASSISTANT_CHILD_HELPER
  # on Windows.
  add_executable(toolbox_assistant_agent_claude_smoke_test
    tests/claude_smoke_test.cpp
    src/claude_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_claude_smoke_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_claude_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_claude_smoke_test PRIVATE
    ASSISTANT_CHILD_HELPER="$<TARGET_FILE:toolbox_assistant_agent_child_helper>"
  )
  add_dependencies(toolbox_assistant_agent_claude_smoke_test toolbox_assistant_agent_child_helper)
  target_compile_features(toolbox_assistant_agent_claude_smoke_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentClaudeSmokeTest COMMAND toolbox_assistant_agent_claude_smoke_test)

  # Command-line lock, fake-CLI resume/translation tests and the codex_stream.hpp parser, plus the
  # live opt-in half (ASSISTANT_CODEX_SMOKE=1), guarded like ClaudeSmoke.*. Needs
  # ASSISTANT_CHILD_HELPER like claude_smoke_test.
  add_executable(toolbox_assistant_agent_codex_backend_test
    tests/codex_backend_test.cpp
    src/codex_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_codex_backend_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_codex_backend_test PRIVATE
    ASSISTANT_CHILD_HELPER="$<TARGET_FILE:toolbox_assistant_agent_child_helper>"
  )
  add_dependencies(toolbox_assistant_agent_codex_backend_test toolbox_assistant_agent_child_helper)
  target_compile_features(toolbox_assistant_agent_codex_backend_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexBackendTest COMMAND toolbox_assistant_agent_codex_backend_test)

  # Feature-awareness benchmark runner: asks the real CLI the questions in
  # docs/benchmarks/features/questions.json against each fixture and writes JSONL for score.py.
  # Opt-in (ASSISTANT_FEATURES_EVAL=claude|codex); skipped otherwise, so CI stays offline and free.
  add_executable(toolbox_assistant_agent_features_eval_test
    tests/features_eval_test.cpp
    src/claude_backend.cpp
    src/codex_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_features_eval_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_features_eval_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_features_eval_test PRIVATE
    ASSISTANT_FEATURES_BENCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/benchmarks/features"
  )
  target_compile_features(toolbox_assistant_agent_features_eval_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentFeaturesEvalTest COMMAND toolbox_assistant_agent_features_eval_test)

  # Fixtures under tests/fixtures/codex_sessions/ are copied to a temp dir per test; nothing reads
  # or writes the real ~/.codex store. plotjuggler_sdk::base is for PJ::sdk::getEnv.
  add_executable(toolbox_assistant_agent_codex_sessions_test
    tests/codex_sessions_test.cpp
    src/codex_sessions.cpp
    src/claude_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_codex_sessions_test PRIVATE
    plotjuggler_sdk::base pj_test_support nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_sessions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_codex_sessions_test PRIVATE
    ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codex_sessions"
  )
  target_compile_features(toolbox_assistant_agent_codex_sessions_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexSessionsTest COMMAND toolbox_assistant_agent_codex_sessions_test)

  # cli_locator.hpp is header-only, so no source is listed.
  add_executable(toolbox_assistant_agent_cli_locator_test
    tests/cli_locator_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_cli_locator_test PRIVATE
    plotjuggler_sdk::plugin_sdk pj_test_support nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_cli_locator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_cli_locator_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCliLocatorTest COMMAND toolbox_assistant_agent_cli_locator_test)

  # Fixture is a synthetic cache (tests/fixtures/codex_models_cache.json). plotjuggler_sdk::base is
  # for PJ::sdk::getEnv, used by codex_sessions.cpp.
  add_executable(toolbox_assistant_agent_codex_models_test
    tests/codex_models_test.cpp
    src/codex_models.cpp
    src/codex_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_codex_models_test PRIVATE
    plotjuggler_sdk::base pj_test_support nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_models_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_codex_models_test PRIVATE
    ASSISTANT_CODEX_MODELS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codex_models_cache.json"
  )
  target_compile_features(toolbox_assistant_agent_codex_models_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexModelsTest COMMAND toolbox_assistant_agent_codex_models_test)

  # subprocess.hpp and private_file.hpp against the real OS.
  add_executable(toolbox_assistant_agent_platform_test
    tests/subprocess_test.cpp
    tests/private_file_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_platform_test PRIVATE
    plotjuggler_sdk::base pj_test_support GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_platform_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_platform_test PRIVATE
    ASSISTANT_CHILD_HELPER="$<TARGET_FILE:toolbox_assistant_agent_child_helper>"
  )
  add_dependencies(toolbox_assistant_agent_platform_test toolbox_assistant_agent_child_helper)
  target_compile_features(toolbox_assistant_agent_platform_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentPlatformTest COMMAND toolbox_assistant_agent_platform_test)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(toolbox_assistant_agent_plugin
  REQUIRED_EXPORTS PJ_get_toolbox_vtable PJ_get_dialog_vtable)
