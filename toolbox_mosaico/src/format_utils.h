// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace mosaico {

// Human-readable byte count, matching PJ3 format_utils.h: 1024-based units
// (KiB/MiB/GiB displayed as "KB"/"MB"/"GB"), 1 decimal for KB/MB/GB, integer
// for raw bytes, empty for non-positive.
[[nodiscard]] inline std::string formatBytes(std::int64_t bytes) {
  if (bytes <= 0) {
    return {};
  }
  constexpr std::int64_t kKB = 1024;
  constexpr std::int64_t kMB = 1024 * kKB;
  constexpr std::int64_t kGB = 1024 * kMB;
  char buf[32];
  const double b = static_cast<double>(bytes);
  if (bytes >= kGB) {
    std::snprintf(buf, sizeof(buf), "%.1f GB", b / static_cast<double>(kGB));
  } else if (bytes >= kMB) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", b / static_cast<double>(kMB));
  } else if (bytes >= kKB) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", b / static_cast<double>(kKB));
  } else {
    std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
  }
  return std::string(buf);
}

}  // namespace mosaico
