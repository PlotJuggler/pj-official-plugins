// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for the sequence/topic name filter (PJ3 substring + ".*" regex).

#include "../src/name_filter.h"

#include "gtest/gtest.h"

using mosaico::containsCaseInsensitive;
using mosaico::nameMatches;

TEST(NameFilter, EmptyPatternMatchesEverything) {
  EXPECT_TRUE(nameMatches("anything", "", false));
  EXPECT_TRUE(nameMatches("anything", "", true));
}

TEST(NameFilter, SubstringIsCaseInsensitive) {
  EXPECT_TRUE(nameMatches("MyRobotSequence", "robot", false));
  EXPECT_TRUE(nameMatches("MyRobotSequence", "ROBOT", false));
  EXPECT_FALSE(nameMatches("MyRobotSequence", "drone", false));
}

TEST(NameFilter, RegexMode) {
  EXPECT_TRUE(nameMatches("camera_front", "^camera_.*", true));
  EXPECT_TRUE(nameMatches("camera_FRONT", "front$", true));  // case-insensitive
  EXPECT_FALSE(nameMatches("lidar_top", "^camera_.*", true));
}

TEST(NameFilter, SubstringDoesNotInterpretRegexMetachars) {
  // In substring mode, "." is literal — must not match an arbitrary char.
  EXPECT_FALSE(nameMatches("abc", "a.c", false));
  EXPECT_TRUE(nameMatches("a.c", "a.c", false));
}

TEST(NameFilter, InvalidRegexMatchesNothing) {
  // Unbalanced bracket → invalid regex → no match (filter visibly takes effect).
  EXPECT_FALSE(nameMatches("anything", "[unterminated", true));
}

TEST(NameFilter, ContainsCaseInsensitiveDirect) {
  EXPECT_TRUE(containsCaseInsensitive("Hello", "ell"));
  EXPECT_TRUE(containsCaseInsensitive("Hello", ""));
  EXPECT_FALSE(containsCaseInsensitive("Hello", "xyz"));
}
