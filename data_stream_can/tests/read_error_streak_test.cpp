#include "read_error_streak.hpp"

#include <gtest/gtest.h>

namespace {

using can_stream::ReadErrorStreak;

TEST(ReadErrorStreak, NineErrorsAreNotFatal) {
  ReadErrorStreak streak;
  for (int i = 0; i < 9; ++i) {
    EXPECT_FALSE(streak.onReadError()) << "error #" << i + 1;
  }
}

TEST(ReadErrorStreak, TheTenthConsecutiveErrorIsFatal) {
  ReadErrorStreak streak;
  for (int i = 0; i < 9; ++i) {
    streak.onReadError();
  }
  EXPECT_TRUE(streak.onReadError());
}

TEST(ReadErrorStreak, AFrameResetsTheStreak) {
  ReadErrorStreak streak;
  for (int i = 0; i < 9; ++i) {
    streak.onReadError();
  }
  streak.onFrame();
  for (int i = 0; i < 9; ++i) {
    EXPECT_FALSE(streak.onReadError()) << "error #" << i + 1 << " after the frame";
  }
}

}  // namespace
