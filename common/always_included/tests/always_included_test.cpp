#include <gtest/gtest.h>

#include <pj_always_included/always_included.hpp>

using PJ::AlwaysIncluded::matches;

TEST(AlwaysIncluded, MatchesRos2TfAndFoxgloveFrameTransform) {
  EXPECT_TRUE(matches("tf2_msgs/msg/TFMessage", "cdr"));
  EXPECT_TRUE(matches("foxglove.FrameTransform", "protobuf"));
}

TEST(AlwaysIncluded, RejectsWrongEncodingOrLookAlikeSchema) {
  EXPECT_FALSE(matches("tf2_msgs/msg/TFMessage", "json"));
  EXPECT_FALSE(matches("foxglove.FrameTransform", "json"));
  EXPECT_FALSE(matches("foxglove.FrameTransforms", "protobuf"));  // no parser turns it into transforms
  EXPECT_FALSE(matches("tf2_msgs/TFMessage", "ros1"));
  EXPECT_FALSE(matches("geometry_msgs/msg/TransformStamped", "cdr"));
  EXPECT_FALSE(matches("", ""));
}

TEST(AlwaysIncluded, MatchesRos2ChecksTheTypeAsCdr) {
  EXPECT_TRUE(PJ::AlwaysIncluded::matchesRos2("tf2_msgs/msg/TFMessage"));
  EXPECT_FALSE(PJ::AlwaysIncluded::matchesRos2("foxglove.FrameTransform"));
  EXPECT_FALSE(PJ::AlwaysIncluded::matchesRos2("std_msgs/msg/String"));
}
