#include <gtest/gtest.h>

#include "lsl_conversions.hpp"

using pj_lsl::TimestampMode;

TEST(TimestampMode, ParseAndRoundTrip) {
  EXPECT_EQ(pj_lsl::parseTimestampMode("sync"), TimestampMode::kSync);
  EXPECT_EQ(pj_lsl::parseTimestampMode("raw"), TimestampMode::kRaw);
  EXPECT_EQ(pj_lsl::parseTimestampMode("receiver"), TimestampMode::kReceiver);
  EXPECT_EQ(pj_lsl::parseTimestampMode("bogus"), TimestampMode::kSync);  // default

  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kSync), "sync");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kRaw), "raw");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kReceiver), "receiver");
}
