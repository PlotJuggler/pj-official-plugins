// FFmpeg `creation_time` tags flow through the SDK's PJ::parseIso8601Utc;
// these vectors pin the plugin-relevant behavior of that contract.
#include <gtest/gtest.h>

#include <cstdint>
#include <pj_base/time_format.hpp>

namespace {

// 2026-05-21T10:00:00 UTC == 1779357600 epoch seconds (verified independently).
constexpr int64_t kExpectedSec = 1779357600LL;
constexpr int64_t kExpectedNs = kExpectedSec * 1'000'000'000LL;

}  // namespace

TEST(ParseIso8601, FfmpegCreationTimeFormat) {
  // The exact format ffmpeg writes:
  //   ffmpeg -metadata creation_time="$(date -u +'%Y-%m-%dT%H:%M:%S.%6NZ')"
  const auto ns = PJ::parseIso8601Utc("2026-05-21T10:00:00.123456Z");
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kExpectedNs + 123'456'000LL);
}

TEST(ParseIso8601, AcceptsWithoutFractionalSeconds) {
  const auto ns = PJ::parseIso8601Utc("2026-05-21T10:00:00Z");
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kExpectedNs);
}

TEST(ParseIso8601, AcceptsMissingTimezoneAsUtc) {
  // The SDK parser reads a timezone-less stamp as UTC (FFmpeg always writes
  // the trailing Z in practice, so runtime behavior is unchanged).
  const auto ns = PJ::parseIso8601Utc("2026-05-21T10:00:00.123456");
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kExpectedNs + 123'456'000LL);
}

TEST(ParseIso8601, RejectsEmpty) {
  EXPECT_FALSE(PJ::parseIso8601Utc("").has_value());
}

TEST(ParseIso8601, RejectsGarbage) {
  EXPECT_FALSE(PJ::parseIso8601Utc("not-a-date").has_value());
}

TEST(ParseIso8601, RejectsImpossibleDate) {
  EXPECT_FALSE(PJ::parseIso8601Utc("2026-13-01T00:00:00Z").has_value());
}
