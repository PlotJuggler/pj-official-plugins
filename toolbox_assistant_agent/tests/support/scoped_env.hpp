// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace assistant_agent::testing {

// Saves/restores one environment variable around a test, so code that reads
// HOME / XDG_STATE_HOME / CLAUDE_CONFIG_DIR can be exercised against a
// controlled value without leaking into any other test in the binary.
// `value == nullptr` unsets it.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name); old != nullptr) {
      had_old_ = true;
      old_ = old;
    }
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnv() {
    if (had_old_) {
      setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
  bool had_old_ = false;
  std::string old_;
};

// A fresh throwaway directory under the system temp dir; empty (with a test
// failure recorded) when it cannot be created. The caller removes it.
inline std::filesystem::path makeTempDir(const char* prefix) {
  std::string tmpl = (std::filesystem::temp_directory_path() / (std::string(prefix) + "XXXXXX")).string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* made = mkdtemp(buf.data());
  if (made == nullptr) {
    ADD_FAILURE() << "mkdtemp failed: " << std::strerror(errno);
    return {};
  }
  return std::filesystem::path(made);
}

}  // namespace assistant_agent::testing
