#include "mp4_iso8601.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace pj_mp4;

namespace {

// 2026-05-21T10:00:00 UTC == 1779357600 epoch seconds (verified independently).
constexpr int64_t kExpectedSec = 1779357600LL;
constexpr int64_t kExpectedNs = kExpectedSec * 1'000'000'000LL;

}  // namespace

TEST(ParseIso8601, FfmpegCreationTimeFormat) {
  // The exact format ffmpeg writes:
  //   ffmpeg -metadata creation_time="$(date -u +'%Y-%m-%dT%H:%M:%S.%6NZ')"
  const auto ns = parseIso8601ToEpochNs("2026-05-21T10:00:00.123456Z");
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kExpectedNs + 123'456'000LL);
}

TEST(ParseIso8601, AcceptsWithoutFractionalSeconds) {
  const auto ns = parseIso8601ToEpochNs("2026-05-21T10:00:00Z");
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, kExpectedNs);
}

TEST(ParseIso8601, RejectsMissingTimezone) {
  // Without trailing Z we can't be sure it is UTC, so refuse.
  EXPECT_FALSE(parseIso8601ToEpochNs("2026-05-21T10:00:00.123456").has_value());
}

TEST(ParseIso8601, RejectsEmpty) {
  EXPECT_FALSE(parseIso8601ToEpochNs("").has_value());
}

TEST(ParseIso8601, RejectsGarbage) {
  EXPECT_FALSE(parseIso8601ToEpochNs("not-a-date").has_value());
}

TEST(ParseIso8601, RejectsImpossibleDate) {
  EXPECT_FALSE(parseIso8601ToEpochNs("2026-13-01T00:00:00Z").has_value());
}
