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

add_library(toolbox_assistant_agent_plugin SHARED
  src/assistant_toolbox.cpp
  src/assistant_dialog.cpp
  src/settings_store.cpp
  src/tools.cpp
  src/ollama_backend.cpp
  src/mcp_http_server.cpp
  src/claude_backend.cpp
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

  # OllamaBackend against an in-test ix::HttpServer impersonating Ollama's
  # /api/chat (incl. a tool_calls round) and /api/tags.
  add_executable(toolbox_assistant_agent_ollama_backend_test
    tests/ollama_backend_test.cpp
    src/ollama_backend.cpp
    src/tools.cpp
  )
  target_link_libraries(toolbox_assistant_agent_ollama_backend_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_ollama_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_ollama_backend_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentOllamaBackendTest COMMAND toolbox_assistant_agent_ollama_backend_test)

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

  # Live Claude Code smoke — spawns the real `claude` CLI + MCP server. Opt-in
  # (ASSISTANT_CLAUDE_SMOKE=1); skips otherwise so CI stays offline.
  add_executable(toolbox_assistant_agent_claude_smoke_test
    tests/claude_smoke_test.cpp
    src/claude_backend.cpp
    src/mcp_http_server.cpp
    src/tools.cpp
  )
  target_link_libraries(toolbox_assistant_agent_claude_smoke_test PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket GTest::gtest_main
  )
  target_include_directories(toolbox_assistant_agent_claude_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(toolbox_assistant_agent_claude_smoke_test PRIVATE cxx_std_20)
  add_test(NAME ToolboxAssistantAgentClaudeSmokeTest COMMAND toolbox_assistant_agent_claude_smoke_test)
endif()
