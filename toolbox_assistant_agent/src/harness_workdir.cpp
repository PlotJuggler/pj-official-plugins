// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "harness_workdir.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>  // mkdir/lstat for the stable work dir
#include <unistd.h>    // getuid
#endif

#include <cstdlib>

namespace assistant_agent {

bool ensureWorkDir(std::string& work_dir, std::string& err) {
  if (!work_dir.empty()) {
    return true;
  }
  // Fail closed on every path below: running in the inherited cwd would
  // silently hand the panel whatever project context PlotJuggler was launched
  // from.
  std::string base;
  if (const char* state_home = std::getenv("XDG_STATE_HOME"); state_home != nullptr && state_home[0] != '\0') {
    base = state_home;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    base = std::string(home) + "/.local/state";
  } else {
    err = "could not resolve the assistant's working directory (no XDG_STATE_HOME or HOME)";
    return false;
  }
  const std::string dir = base + "/pj-assistant-cli";
  // Best-effort parents (XDG_STATE_HOME normally exists); the leaf must end up
  // a real 0700 directory of ours — a symlink planted there would redirect the
  // CLI's session state, so lstat, not stat.
  mkdir(base.c_str(), 0700);
  mkdir(dir.c_str(), 0700);
  struct stat st{};
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    err = "could not create the assistant's working directory (" + dir + ")";
    return false;
  }
  work_dir = dir;
  return true;
}

}  // namespace assistant_agent
