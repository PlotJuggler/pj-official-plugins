// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <gtest/gtest.h>

#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <vector>

#include "llm_backend.hpp"    // BackendEvent, LlmBackend, TurnTools
#include "tool_registry.hpp"  // ToolRegistry, ToolContext

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // mkstemp/close/chmod for the fake CLI script

#include <fstream>
#endif

namespace assistant_agent::testing {

// The first Error event, or nullptr — every fake-CLI resume/translation test
// (claude_smoke_test.cpp, codex_backend_test.cpp) asks this same question.
inline const BackendEvent* firstError(const std::vector<BackendEvent>& events) {
  for (const auto& e : events) {
    if (e.kind == BackendEvent::Kind::Error) {
      return &e;
    }
  }
  return nullptr;
}

#if defined(__unix__) || defined(__APPLE__)

// A stand-in for a headless CLI: drains stdin (so a payload writer never
// SIGPIPEs), prints `stdout_lines` as NDJSON, then exits `exit_code`. The
// general form both claude_smoke_test.cpp and codex_backend_test.cpp used to
// write independently — Claude's own `writeFakeCli(result_line)` is now a
// thin wrapper over this passing `{init_record, result_line}` and exit 1.
inline std::string writeFakeCliScript(const std::vector<std::string>& stdout_lines, int exit_code) {
  char tmpl[] = "/tmp/pj_assistant_fake_cli_XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    return {};
  }
  close(fd);
  {
    std::ofstream script(tmpl);
    script << "#!/bin/sh\n"
           << "cat >/dev/null\n";  // drain stdin first, or a non-reading script SIGPIPEs the writer
    if (!stdout_lines.empty()) {
      script << "cat <<'EOF'\n";
      for (const auto& line : stdout_lines) {
        script << line << "\n";
      }
      script << "EOF\n";
    }
    script << "exit " << exit_code << "\n";
  }
  chmod(tmpl, 0700);
  return tmpl;
}

#endif  // POSIX

// The shared body of ClaudeSmoke.ListTopicsThroughMcp and
// CodexSmoke.ListTopicsThroughMcp: drive one live turn asking the model to
// call list_topics, against a fake host with exactly one topic, and check it
// did. Each call site keeps its own ASSISTANT_*_SMOKE env-var gate and
// backend construction (the CLI path override differs per backend) and hands
// the already-built backend here. `label` names the backend in the failure
// message only ("Claude"/"Codex").
inline void runListTopicsSmoke(LlmBackend& backend, const char* label) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/imu/accel");
  store.addField("/imu/accel", "x", {0}, {1.0});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());

  TurnTools tools;
  tools.registry = &reg;
  // Direct execution (no GuiExecutor): the MCP server thread runs the tool.
  tools.invoke = [&](const std::string& n, const nlohmann::json& a) { return reg.execute(n, a, ctx); };

  std::vector<BackendEvent> events;
  std::mutex mu;
  backend.sendUserMessage(
      "Call the list_topics tool and tell me the exact topic name it returns.", tools, [&](BackendEvent e) {
        std::lock_guard<std::mutex> lk(mu);
        std::cerr << "[event " << static_cast<int>(e.kind) << "] " << e.text << "\n";
        events.push_back(std::move(e));
      });

  bool complete = false;
  bool mentioned_topic = false;
  bool called_tool = false;
  for (const auto& e : events) {
    if (e.kind == BackendEvent::Kind::TurnComplete) {
      complete = true;
    }
    if (e.kind == BackendEvent::Kind::ToolActivity && e.text.find("list_topics") != std::string::npos) {
      called_tool = true;
    }
    if (e.text.find("/imu/accel") != std::string::npos) {
      mentioned_topic = true;
    }
  }
  EXPECT_TRUE(complete) << "backend never signalled TurnComplete";
  EXPECT_TRUE(called_tool) << label << " did not call list_topics via MCP";
  EXPECT_TRUE(mentioned_topic) << "answer did not mention the topic the tool returned";
}

}  // namespace assistant_agent::testing
