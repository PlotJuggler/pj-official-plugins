// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "harness_workdir.hpp"

#include <filesystem>
#include <pj_base/sdk/platform.hpp>
#include <system_error>

namespace assistant_agent {

bool ensureWorkDir(std::string& work_dir, std::string& err) {
  if (!work_dir.empty()) {
    return true;
  }
  // PJ::sdk::userDataDir() is the SDK's own answer to "where does a plugin keep
  // per-user state" -- %LOCALAPPDATA% on Windows, Application Support on macOS,
  // XDG data home on Linux, and never empty. toolbox_transform_editor resolves
  // its snippet library the same way. Using it is what makes this function work
  // on every platform without a #if.
  //
  // Nothing of ours is ever written in here: the directory exists so the CLI
  // runs somewhere fixed instead of inheriting whatever project PlotJuggler was
  // launched from, and so its own session store (~/.claude, ~/.codex) is keyed
  // by a cwd that does not change between runs. It still gets owner-only
  // permissions, since it is the only thing naming this user's conversations.
  const std::filesystem::path dir = PJ::sdk::userDataDir() / "pj-assistant-cli";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) {
    err = "could not create the assistant's working directory (" + dir.string() + ")";
    return false;
  }
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);  // best effort
  work_dir = dir.string();
  return true;
}

}  // namespace assistant_agent
