// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <string>

#include "llm_backend.hpp"  // BackendTestResult
#include "subprocess.hpp"

namespace assistant_agent {

// The connectivity probe every headless-CLI backend runs: `<cli_path>
// --version` exits 0. Shared by ClaudeBackend::testConnection and
// CodexBackend::testConnection, which differ only in which CLI they name in
// the failure message (`display_name`, e.g. "Claude" / "Codex").
[[nodiscard]] inline BackendTestResult probeCliVersion(const std::string& cli_path, const std::string& display_name) {
  std::atomic<bool> no_cancel{false};
  std::string version;
  auto res = runProcess(
      {cli_path, "--version"}, /*stdin_data=*/{}, [&](const std::string& chunk) { version += chunk; }, no_cancel);
  if (!res.spawned) {
    return {false, "cannot run '" + cli_path + "': " + res.error};
  }
  if (res.exit_code != 0) {
    return {
        false, "'" + cli_path + " --version' exited " + std::to_string(res.exit_code) + " (is the " + display_name +
                   " CLI installed and logged in?)"};
  }
  // Trim trailing newline for a tidy transcript line.
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
    version.pop_back();
  }
  return {true, "found " + version};
}

}  // namespace assistant_agent
