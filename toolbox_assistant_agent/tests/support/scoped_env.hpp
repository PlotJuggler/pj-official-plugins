// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <pj_base/sdk/platform.hpp>
#include <random>
#include <string>

namespace assistant_agent::testing {

// Saves/restores one environment variable around a test, so code that reads
// HOME / XDG_STATE_HOME / CLAUDE_CONFIG_DIR can be exercised against a
// controlled value without leaking into any other test in the binary.
// `value == nullptr` unsets it.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const std::optional<std::string> old = PJ::sdk::getEnv(name)) {
      had_old_ = true;
      old_ = *old;
    }
    setEnv(name_, value);
  }

  // std::filesystem::path::c_str() is const wchar_t* on Windows, so a path
  // cannot reach the const char* constructor there. Take the path itself and
  // narrow it here, once.
  ScopedEnv(const char* name, const std::filesystem::path& value) : ScopedEnv(name, value.string().c_str()) {}

  ~ScopedEnv() {
    setEnv(name_, had_old_ ? old_.c_str() : nullptr);
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  // setenv/unsetenv do not exist on MSVC; _putenv_s with an empty value is how
  // it removes a variable, which is also why an empty value cannot be
  // distinguished from an unset one there -- the same collapse PJ::sdk::getEnv
  // already makes on every platform.
  static void setEnv(const std::string& name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value != nullptr ? value : "");
#else
    if (value != nullptr) {
      setenv(name.c_str(), value, 1);
    } else {
      unsetenv(name.c_str());
    }
#endif
  }

  std::string name_;
  bool had_old_ = false;
  std::string old_;
};

// Makes the CRT re-read TZ after a ScopedEnv changed it. MSVC spells tzset()
// with a leading underscore.
inline void refreshTimezone() {
#if defined(_WIN32)
  _tzset();
#else
  tzset();
#endif
}

// A fresh throwaway directory under the system temp dir; empty (with a test
// failure recorded) when it cannot be created. The caller removes it.
inline std::filesystem::path makeTempDir(const char* prefix) {
  // create_directory() reports whether THIS call created the directory, so the
  // retry loop is a race-free stand-in for mkdtemp(), which MSVC lacks.
  std::mt19937_64 rng(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::filesystem::path candidate =
        std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(rng()));
    std::error_code ec;
    if (std::filesystem::create_directory(candidate, ec)) {
      return candidate;
    }
    if (ec) {
      ADD_FAILURE() << "could not create a temp dir under " << std::filesystem::temp_directory_path() << ": "
                    << ec.message();
      return {};
    }
  }
  ADD_FAILURE() << "could not find a free temp dir name for prefix " << prefix;
  return {};
}

}  // namespace assistant_agent::testing
