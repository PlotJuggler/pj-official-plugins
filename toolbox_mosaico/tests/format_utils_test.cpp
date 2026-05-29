// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for formatBytes. PJ3 parity (format_utils.h): 1024-based units
// (gap #3), 1 decimal for KB/MB/GB, integer for raw bytes, empty for
// non-positive.

#include "../src/format_utils.h"

#include <cstdint>

#include "gtest/gtest.h"

using mosaico::formatBytes;

TEST(FormatBytes, NonPositiveIsEmpty) {
  EXPECT_EQ(formatBytes(0), "");
  EXPECT_EQ(formatBytes(-1), "");
}

TEST(FormatBytes, RawBytesAreInteger) {
  EXPECT_EQ(formatBytes(1), "1 B");
  EXPECT_EQ(formatBytes(512), "512 B");
  EXPECT_EQ(formatBytes(1023), "1023 B");
}

TEST(FormatBytes, KilobytesAre1024Based) {
  // 1024 B = 1.0 KB (NOT 1000-based, which would print "1.0 KB" at 1000 and
  // "1.0 KB" only at 1024 here).
  EXPECT_EQ(formatBytes(1024), "1.0 KB");
  // 1500 B / 1024 = 1.46 -> "1.5 KB" (1000-based would give "1.5 KB" too, so
  // pick a value that disambiguates):
  // 1000 B is BELOW the 1024 threshold -> still raw bytes (the key 1024 vs 1000
  // distinction).
  EXPECT_EQ(formatBytes(1000), "1000 B");
}

TEST(FormatBytes, MegabytesAre1024Based) {
  constexpr std::int64_t kMB = 1024 * 1024;
  EXPECT_EQ(formatBytes(kMB), "1.0 MB");
  // 1,000,000 B is below the 1 MiB threshold (1,048,576) -> still KB, not MB.
  // 1,000,000 / 1024 = 976.6 -> "976.6 KB".
  EXPECT_EQ(formatBytes(1'000'000), "976.6 KB");
}

TEST(FormatBytes, GigabytesAre1024Based) {
  constexpr std::int64_t kGB = 1024LL * 1024 * 1024;
  EXPECT_EQ(formatBytes(kGB), "1.0 GB");
  // 1,500,000,000 / 2^30 = 1.397 -> "1.4 GB".
  EXPECT_EQ(formatBytes(1'500'000'000LL), "1.4 GB");
}

TEST(FormatBytes, BoundaryJustBelowKilobyteStaysBytes) {
  // The 1000-based implementation would have switched to "1.0 KB" at 1000;
  // 1024-based keeps it as raw bytes until 1024.
  EXPECT_EQ(formatBytes(1023), "1023 B");
  EXPECT_EQ(formatBytes(1024), "1.0 KB");
}
