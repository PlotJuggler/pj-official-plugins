find_package(nlohmann_json REQUIRED)
find_package(fmt REQUIRED)
find_package(ixwebsocket REQUIRED)

# Fix ixwebsocket include dirs when built inside the PJ4 tree without conan toolchain.
if(DEFINED IXWEBSOCKET_INCLUDE_DIR AND TARGET ixwebsocket::ixwebsocket)
  set_target_properties(ixwebsocket::ixwebsocket PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${IXWEBSOCKET_INCLUDE_DIR}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# Shared across the plugin target and every test/bench executable that links a
# real backend (assistant_dialog_test, claude_smoke_test, codex_backend_test,
# the bench) — NOT an OBJECT library: the plugin target compiles with
# ${PJ_WARNING_FLAGS} and the tests do not, so each target must still compile
# its own copies of these translation units under its own flags.
set(ASSISTANT_AGENT_COMMON_SOURCES
  src/mcp_http_server.cpp
  src/mcp_loopback.cpp
  src/harness_workdir.cpp
  src/tools.cpp
  src/claude_sessions.cpp
  src/codex_sessions.cpp
  src/codex_models.cpp
)

add_library(toolbox_assistant_agent_plugin SHARED
  src/assistant_toolbox.cpp
  src/assistant_dialog.cpp
  src/conversation_state.cpp
  src/settings_store.cpp
  src/claude_backend.cpp
  src/codex_backend.cpp
  ${ASSISTANT_AGENT_COMMON_SOURCES}
)

target_compile_features(toolbox_assistant_agent_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_assistant_agent_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_assistant_agent_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  fmt::fmt
  ixwebsocket::ixwebsocket
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

pj_embed_manifest(toolbox_assistant_agent_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/assistant_panel_manifest.hpp
  VAR_NAME kAssistantPanelManifest
)

pj_emit_plugin_manifest(toolbox_assistant_agent_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
if(BUILD_TESTING)
  find_package(GTest REQUIRED)

  # ChatSession: transcript rendering + turn-state machine. Pure, std-only.
  add_executable(toolbox_assistant_agent_chat_session_test
    tests/chat_session_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_chat_session_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_chat_session_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_chat_session_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentChatSessionTest COMMAND toolbox_assistant_agent_chat_session_test)

  # UsageLedger: last turn's token counts + the status-label text. Pure.
  add_executable(toolbox_assistant_agent_usage_ledger_test
    tests/usage_ledger_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_usage_ledger_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_usage_ledger_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_usage_ledger_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentUsageLedgerTest COMMAND toolbox_assistant_agent_usage_ledger_test)

  # SettingsStore adapter over pj.settings.v1 (fake in-memory settings host).
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

  # ConversationState: the persisted conversation (settings-store round-trip,
  # caps, defensive parsing) plus the fnv1a hash it shares with ClaudeMemory.
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

  # ClaudeSessions: the harness's own ~/.claude/projects/<slug>/*.jsonl store —
  # listing/ordering/titles, transcript replay, the catalog-prefix strip, and
  # tolerance of corrupt lines / unknown record types. Fixtures under
  # tests/fixtures/sessions/ are copied to a temp dir per test (see
  # ClaudeSessionsTest in the test file); nothing here reads or writes the real
  # ~/.claude store. std + nlohmann only.
  add_executable(toolbox_assistant_agent_claude_sessions_test
    tests/claude_sessions_test.cpp
    src/claude_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_claude_sessions_test PRIVATE
    nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_claude_sessions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_claude_sessions_test PRIVATE
    ASSISTANT_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sessions"
  )
  target_compile_features(toolbox_assistant_agent_claude_sessions_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentClaudeSessionsTest COMMAND toolbox_assistant_agent_claude_sessions_test)

  # ModelPicker: the settings combo's index<->value mapping (CLI default /
  # Custom.../listed choices), both directions. Pure, std-only.
  add_executable(toolbox_assistant_agent_model_picker_test
    tests/model_picker_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_model_picker_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_model_picker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_model_picker_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentModelPickerTest COMMAND toolbox_assistant_agent_model_picker_test)

  # SeriesStats: stats + min/max-preserving bucket decimation. Pure, std-only.
  add_executable(toolbox_assistant_agent_series_stats_test
    tests/series_stats_test.cpp
  )
  target_link_libraries(toolbox_assistant_agent_series_stats_test PRIVATE GTest::gtest_main)
  target_include_directories(toolbox_assistant_agent_series_stats_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_series_stats_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentSeriesStatsTest COMMAND toolbox_assistant_agent_series_stats_test)

  # ToolRegistry executors against a fake C-ABI toolbox host (read tools) and a
  # recording fake data-processors host (write tools).
  add_executable(toolbox_assistant_agent_tool_registry_test
    tests/tool_registry_test.cpp
    src/tools.cpp
  )
  target_link_libraries(toolbox_assistant_agent_tool_registry_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_tool_registry_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_tool_registry_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentToolRegistryTest COMMAND toolbox_assistant_agent_tool_registry_test)

  # Claude stream-json parser + NDJSON splitter. Pure, std + nlohmann only.
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

  # MCP HTTP server driven by a loopback ix::HttpClient simulating Claude's
  # JSON-RPC: initialize / tools/list / tools/call, plus bearer-token auth.
  add_executable(toolbox_assistant_agent_mcp_server_test
    tests/mcp_server_test.cpp
    src/mcp_http_server.cpp
    src/tools.cpp
  )
  target_link_libraries(toolbox_assistant_agent_mcp_server_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_mcp_server_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_mcp_server_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentMcpServerTest COMMAND toolbox_assistant_agent_mcp_server_test)

  # AssistantDialog end-to-end: the conversations drawer against a REAL
  # ClaudeBackend pointed at a throwaway HOME (fixtures copied into its
  # ~/.claude/projects/<slug>/ store) — opening the drawer, switching to a
  # resumed conversation, and deleting the active one. No `claude` CLI is
  # spawned: none of these paths call sendUserMessage/testConnection.
  add_executable(toolbox_assistant_agent_assistant_dialog_test
    tests/assistant_dialog_test.cpp
    src/assistant_dialog.cpp
    src/claude_backend.cpp
    src/codex_backend.cpp
    src/conversation_state.cpp
    src/settings_store.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_assistant_dialog_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
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

  # Live Claude Code smoke — spawns the real `claude` CLI + MCP server. Opt-in
  # (ASSISTANT_CLAUDE_SMOKE=1); skips otherwise so CI stays offline.
  add_executable(toolbox_assistant_agent_claude_smoke_test
    tests/claude_smoke_test.cpp
    src/claude_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_claude_smoke_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_claude_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_claude_smoke_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentClaudeSmokeTest COMMAND toolbox_assistant_agent_claude_smoke_test)

  # Codex backend: command-line lock (the "no --tools """ spine), the fake-CLI
  # resume/translation tests, and the parser (codex_stream.hpp) — mirrors
  # claude_smoke_test.cpp's offline half plus stream_json_test.cpp, folded into
  # one target per the brief. The live opt-in half (ASSISTANT_CODEX_SMOKE=1)
  # lives in the same file, guarded the same way ClaudeSmoke.* is.
  add_executable(toolbox_assistant_agent_codex_backend_test
    tests/codex_backend_test.cpp
    src/codex_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_codex_backend_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_codex_backend_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexBackendTest COMMAND toolbox_assistant_agent_codex_backend_test)

  # CodexSessions: the CLI's own ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl
  # store — listing/ordering/titles, transcript replay, the cwd filter, and
  # tolerance of corrupt lines, exactly like claude_sessions_test.cpp. Fixtures under
  # tests/fixtures/codex_sessions/ are copied to a temp dir per test; nothing
  # here reads or writes the real ~/.codex store.
  add_executable(toolbox_assistant_agent_codex_sessions_test
    tests/codex_sessions_test.cpp
    src/codex_sessions.cpp
    src/claude_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_codex_sessions_test PRIVATE
    nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_sessions_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_codex_sessions_test PRIVATE
    ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codex_sessions"
  )
  target_compile_features(toolbox_assistant_agent_codex_sessions_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexSessionsTest COMMAND toolbox_assistant_agent_codex_sessions_test)

  # CodexModels: the settings-combo catalog read from the CLI's own
  # models_cache.json -- visibility filtering, priority ordering, label
  # composition, and tolerance of a missing/corrupt file. Fixture is a
  # verbatim copy of a real cache (tests/fixtures/codex_models_cache.json).
  add_executable(toolbox_assistant_agent_codex_models_test
    tests/codex_models_test.cpp
    src/codex_models.cpp
    src/codex_sessions.cpp
  )
  target_link_libraries(toolbox_assistant_agent_codex_models_test PRIVATE
    nlohmann_json::nlohmann_json GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_codex_models_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(toolbox_assistant_agent_codex_models_test PRIVATE
    ASSISTANT_CODEX_MODELS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codex_models_cache.json"
  )
  target_compile_features(toolbox_assistant_agent_codex_models_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentCodexModelsTest COMMAND toolbox_assistant_agent_codex_models_test)

  # Latency bench — times real turns to count round-trips and separate model time
  # from our own tool time. Opt-in (ASSISTANT_BENCH=1) for the same reason as the
  # smoke above: it spawns the real CLI.
  add_executable(toolbox_assistant_agent_bench
    tests/assistant_bench.cpp
    src/claude_backend.cpp
    ${ASSISTANT_AGENT_COMMON_SOURCES}
  )
  target_link_libraries(toolbox_assistant_agent_bench PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_bench PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentBench COMMAND toolbox_assistant_agent_bench)
endif()
