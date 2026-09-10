// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "harness_workdir.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>  // mkdir/lstat for the stable work dir
#include <unistd.h>    // getuid
#endif

#include <pj_base/sdk/platform.hpp>

namespace assistant_agent {

#if defined(__unix__) || defined(__APPLE__)

bool ensureWorkDir(std::string& work_dir, std::string& err) {
  if (!work_dir.empty()) {
    return true;
  }
  // Fail closed on every path below: running in the inherited cwd would
  // silently hand the panel whatever project context PlotJuggler was launched
  // from.
  std::string base;
  if (const std::optional<std::string> state_home = PJ::sdk::getEnv("XDG_STATE_HOME")) {
    base = *state_home;
  } else if (const std::optional<std::string> home = PJ::sdk::getEnv("HOME")) {
    base = *home + "/.local/state";
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

#else

// The directory this resolves to has to be one WE own, checked with lstat and
// getuid so a planted symlink cannot redirect the CLI's session state. Windows
// expresses that ownership differently, and nothing else in this plugin runs
// there anyway -- runProcess (subprocess.hpp) refuses to spawn the CLI off
// POSIX -- so fail closed with the same shape rather than settle for a weaker
// check.
bool ensureWorkDir(std::string& work_dir, std::string& err) {
  if (!work_dir.empty()) {
    return true;
  }
  err = "the assistant's working directory is only supported on POSIX platforms";
  return false;
}

#endif

}  // namespace assistant_agent
