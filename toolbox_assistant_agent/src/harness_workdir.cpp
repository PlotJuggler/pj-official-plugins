// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "harness_workdir.hpp"

#include <filesystem>
#include <pj_base/sdk/platform.hpp>
#include <system_error>

#include "platform_util.hpp"  // pathToUtf8

namespace assistant_agent {

bool ensureWorkDir(std::string& work_dir, std::string& err) {
  if (!work_dir.empty()) {
    return true;
  }
  // PJ::sdk::userDataDir() is never empty and platform-correct (%LOCALAPPDATA%, Application
  // Support, XDG data home), so no #if is needed.
  //
  // Nothing of ours is written here: it only gives the CLI a fixed cwd, which keys its own
  // session store (~/.claude, ~/.codex). It still gets owner-only permissions, since its name
  // is the only thing tying conversations to this user.
  const std::filesystem::path dir = PJ::sdk::userDataDir() / "pj-assistant-cli";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) {
    err = "could not create the assistant's working directory (" + pathToUtf8(dir) + ")";
    return false;
  }
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);  // best effort
  work_dir = pathToUtf8(dir);
  return true;
}

}  // namespace assistant_agent
