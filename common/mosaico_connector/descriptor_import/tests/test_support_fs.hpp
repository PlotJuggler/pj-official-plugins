// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Shared filesystem test support: RAII temp directory under the system temp
// root, recreated FRESH at construction (a stale dir from a crashed prior run
// is wiped), removed at destruction. `dir_name` is the full leaf name —
// callers derive per-suite uniqueness by prefixing.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace mosaico_test {

struct ScopedTempDir {
  explicit ScopedTempDir(const std::string& dir_name) {
    path = std::filesystem::temp_directory_path() / dir_name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  std::filesystem::path path;
};

}  // namespace mosaico_test
