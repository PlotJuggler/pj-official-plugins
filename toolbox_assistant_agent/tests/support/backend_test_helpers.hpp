// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>
#include <system_error>
#include <vector>

#include "llm_backend.hpp"    // BackendEvent, LlmBackend, TurnTools
#include "platform_util.hpp"  // pathToUtf8, utf8ToPath
#include "tool_registry.hpp"  // ToolRegistry, ToolContext

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // mkstemp/close/chmod for the fake CLI script
#elif defined(_WIN32)
#include <cstdio>  // snprintf
#include <random>

#ifndef ASSISTANT_CHILD_HELPER
#error "ASSISTANT_CHILD_HELPER must be defined by CMake for any target using writeFakeCliScript on Windows"
#endif
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

// Removes a fake CLI writeFakeCliScript created. POSIX's is a single file;
// kept for parity with the Windows overload below so call sites never need
// an #ifdef of their own.
inline void removeFakeCli(const std::string& cli_path) {
  if (!cli_path.empty()) {
    unlink(cli_path.c_str());
  }
}

#elif defined(_WIN32)

// Windows has no shell-script CLI stand-in (subprocess.hpp's runProcess
// refuses anything but a native .exe — see its Windows contract), so the
// fake CLI here is a COPY of tests/support/child_helper.cpp's own binary
// (ASSISTANT_CHILD_HELPER, built as its own CMake target) under a fresh
// name, paired with a `<copy>.exe.script` sidecar file: child_helper checks
// for that file before any normal mode dispatch (see its own header
// comment) and, when present, drains stdin then replays the script's first
// line as its exit code and every later line as an stdout line — exactly
// the behavior the POSIX shell-script version above gives.
inline std::string writeFakeCliScript(const std::vector<std::string>& stdout_lines, int exit_code) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path temp_dir = fs::temp_directory_path(ec);
  if (ec) {
    return {};
  }

  std::random_device rd;
  fs::path exe_path;
  for (int attempt = 0; attempt < 16; ++attempt) {
    char suffix[17];
    std::snprintf(suffix, sizeof(suffix), "%08x%08x", rd(), rd());
    const fs::path candidate = temp_dir / (std::string("pj_assistant_fake_cli_") + suffix + ".exe");
    if (!fs::exists(candidate, ec)) {
      exe_path = candidate;
      break;
    }
  }
  if (exe_path.empty()) {
    return {};
  }

  fs::copy_file(utf8ToPath(ASSISTANT_CHILD_HELPER), exe_path, ec);
  if (ec) {
    return {};
  }

  fs::path script_path = exe_path;
  script_path += ".script";
  std::ofstream script(script_path, std::ios::binary);
  if (!script) {
    return {};
  }
  script << exit_code << "\n";
  for (const auto& line : stdout_lines) {
    script << line << "\n";
  }
  script.close();

  return pathToUtf8(exe_path);
}

// Removes the copy of child_helper.exe AND its `.script` sidecar — a bare
// `remove(exe)` would leave the sidecar behind for another fake CLI's random
// name to never collide with, but there is no reason to keep it around.
// Failures are ignored, like POSIX's unlink() above: this only ever runs in
// a temp directory that gets cleaned up by the OS regardless.
inline void removeFakeCli(const std::string& cli_path) {
  if (cli_path.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path exe_path = utf8ToPath(cli_path);
  std::filesystem::path script_path = exe_path;
  script_path += ".script";
  std::filesystem::remove(exe_path, ec);
  std::filesystem::remove(script_path, ec);
}

#endif  // POSIX / Windows

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
