// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <string>

#include "cli_locator.hpp"  // locateCli, childEnvFor
#include "llm_backend.hpp"  // BackendTestResult
#include "subprocess.hpp"

namespace assistant_agent {

// The message for a CLI that locateCli() could not find anywhere it looked — shared between
// probeCliVersion() and the "cannot find" guard each backend runs before spawning a turn, so the
// wording never drifts between the two paths.
[[nodiscard]] inline std::string cannotFindCliMessage(const std::string& cli_path, const CliLocation& loc) {
  std::string joined;
  for (std::size_t i = 0; i < loc.searched.size(); ++i) {
    if (i != 0) {
      joined += ", ";
    }
    joined += loc.searched[i];
  }
  return "cannot find '" + cli_path + "' — searched: " + joined + ". Set the CLI path in Settings.";
}

// The connectivity probe every headless-CLI backend runs: `<cli_path>
// --version` exits 0. Shared by ClaudeBackend::testConnection and
// CodexBackend::testConnection, which differ only in which CLI they name in
// the failure message (`display_name`, e.g. "Claude" / "Codex").
[[nodiscard]] inline BackendTestResult probeCliVersion(const std::string& cli_path, const std::string& display_name) {
  const CliLocation loc = locateCli(cli_path);
  if (loc.path.empty()) {
    return {false, cannotFindCliMessage(cli_path, loc)};
  }
  std::atomic<bool> no_cancel{false};
  std::string version;
  auto res = runProcess(
      {loc.path, "--version"}, /*stdin_data=*/{}, [&](const std::string& chunk) { version += chunk; }, no_cancel,
      /*working_dir=*/{}, childEnvFor(loc));
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
