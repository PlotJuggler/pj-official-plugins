// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <string>

#include "cli_locator.hpp"  // locateCli, childEnvFor
#include "llm_backend.hpp"  // BackendTestResult
#include "subprocess.hpp"

namespace assistant_agent {

// Message for a CLI that locateCli() could not find; shared by probeCliVersion() and each
// backend's pre-spawn guard. Only reads CliLocation fields, so tests exercise the npm_shim
// branch on Linux by constructing one (locateCli() fills npm_shim on Windows only).
[[nodiscard]] inline std::string cannotFindCliMessage(const std::string& cli_path, const CliLocation& loc) {
  std::string joined;
  for (std::size_t i = 0; i < loc.searched.size(); ++i) {
    if (i != 0) {
      joined += ", ";
    }
    joined += loc.searched[i];
  }
  std::string message = "cannot find '" + cli_path + "' — searched: " + joined + ".";
  if (!loc.npm_shim.empty()) {
    // A `.cmd`/`.ps1` launcher is never run (native .exe only, BatBadBut; see
    // subprocess.hpp). The CLI name picking the install hint is the last path
    // segment minus extension.
    std::string stem = cli_path;
    if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string::npos) {
      stem = stem.substr(slash + 1);
    }
    if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos) {
      stem = stem.substr(0, dot);
    }
    message += " Found only the npm launcher '" + loc.npm_shim +
               "', which the assistant does not run (it would need cmd.exe); install the native build";
    if (stem == "claude") {
      message += " (irm https://claude.ai/install.ps1 | iex)";
    } else if (stem == "codex") {
      message += " (irm https://chatgpt.com/codex/install.ps1 | iex)";
    } else {
      message += " for '" + stem + "'";
    }
    message += " — or set the CLI path in Settings to the .exe.";
  } else {
    message += " Set the CLI path in Settings.";
  }
  return message;
}

// Connectivity probe shared by the CLI backends' testConnection: `<cli_path> --version`
// exits 0. `display_name` only words the failure message.
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
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
    version.pop_back();
  }
  return {true, "found " + version};
}

}  // namespace assistant_agent
