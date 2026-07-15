// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for the shared separate-channel colour collapse.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <pj_pointcloud_color/pointcloud_color.hpp>
#include <vector>

namespace {

using DT = PJ::sdk::PointField::Datatype;
using pj::pointcloud_color::collapseSeparateColorChannels;
using pj::pointcloud_color::normalizeCanonicalColor;
using pj::pointcloud_color::repackPclPackedColor;

PJ::sdk::PointCloud makeCloud(uint32_t point_step, std::vector<PJ::sdk::PointField> fields) {
  PJ::sdk::PointCloud cloud;
  cloud.point_step = point_step;
  cloud.fields = std::move(fields);
  return cloud;
}

// A one-point cloud: 12 bytes of x/y/z (zeroed) then a 4-byte colour field at offset 12,
// whose bytes are given in wire order. Owns its buffer so the repack can read + copy it.
PJ::sdk::PointCloud makePackedColorCloud(const char* field_name, DT datatype, std::array<uint8_t, 4> color_bytes) {
  auto owned = std::make_shared<std::vector<uint8_t>>(16, 0);
  std::copy(color_bytes.begin(), color_bytes.end(), owned->begin() + 12);
  PJ::sdk::PointCloud cloud;
  cloud.width = 1;
  cloud.height = 1;
  cloud.point_step = 16;
  cloud.is_bigendian = false;
  cloud.fields = {
      {"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}, {field_name, 12, datatype, 1}};
  cloud.data = PJ::Span<const uint8_t>(owned->data(), owned->size());
  cloud.anchor = owned;
  return cloud;
}

TEST(PointCloudColor, CollapsesContiguousRgbaToPackedField) {
  auto cloud = makeCloud(
      20, {{"x", 0, DT::kFloat32, 1},
           {"y", 4, DT::kFloat32, 1},
           {"z", 8, DT::kFloat32, 1},
           {"intensity", 12, DT::kFloat32, 1},
           {"red", 16, DT::kUint8, 1},
           {"green", 17, DT::kUint8, 1},
           {"blue", 18, DT::kUint8, 1},
           {"alpha", 19, DT::kUint8, 1}});
  EXPECT_TRUE(collapseSeparateColorChannels(cloud));
  ASSERT_EQ(cloud.fields.size(), 5u);
  EXPECT_EQ(cloud.fields[4].name, "rgba");
  EXPECT_EQ(cloud.fields[4].offset, 16u);
  EXPECT_EQ(cloud.fields[4].datatype, DT::kUint32);
}

TEST(PointCloudColor, CollapsesRgbWithoutAlphaWhenAByteRemains) {
  // r,g,b at 12,13,14 with a 4th byte (point_step 16) -> packed "rgb".
  auto cloud = makeCloud(
      16, {{"x", 0, DT::kFloat32, 1},
           {"y", 4, DT::kFloat32, 1},
           {"z", 8, DT::kFloat32, 1},
           {"red", 12, DT::kUint8, 1},
           {"green", 13, DT::kUint8, 1},
           {"blue", 14, DT::kUint8, 1}});
  EXPECT_TRUE(collapseSeparateColorChannels(cloud));
  ASSERT_EQ(cloud.fields.size(), 4u);
  EXPECT_EQ(cloud.fields[3].name, "rgb");
  EXPECT_EQ(cloud.fields[3].datatype, DT::kUint32);
}

TEST(PointCloudColor, LeavesRgbWithoutAlphaAndNoSpareByte) {
  // r,g,b are the last 3 bytes (point_step 15): no 4th byte -> cannot pack, leave as-is.
  auto cloud = makeCloud(
      15, {{"x", 0, DT::kFloat32, 1},
           {"y", 4, DT::kFloat32, 1},
           {"z", 8, DT::kFloat32, 1},
           {"red", 12, DT::kUint8, 1},
           {"green", 13, DT::kUint8, 1},
           {"blue", 14, DT::kUint8, 1}});
  EXPECT_FALSE(collapseSeparateColorChannels(cloud));
  EXPECT_EQ(cloud.fields.size(), 6u);
}

TEST(PointCloudColor, LeavesNonContiguousChannels) {
  auto cloud = makeCloud(
      20, {{"x", 0, DT::kFloat32, 1},
           {"red", 12, DT::kUint8, 1},
           {"green", 14, DT::kUint8, 1},  // gap
           {"blue", 16, DT::kUint8, 1}});
  EXPECT_FALSE(collapseSeparateColorChannels(cloud));
  EXPECT_EQ(cloud.fields.size(), 4u);
}

TEST(PointCloudColor, LeavesPlainCloudUntouched) {
  auto cloud = makeCloud(
      16, {{"x", 0, DT::kFloat32, 1},
           {"y", 4, DT::kFloat32, 1},
           {"z", 8, DT::kFloat32, 1},
           {"intensity", 12, DT::kFloat32, 1}});
  EXPECT_FALSE(collapseSeparateColorChannels(cloud));
  EXPECT_EQ(cloud.fields.size(), 4u);
}

// --- PCL packed-colour repack (the ROS case) ------------------------------------

TEST(PointCloudColor, RepacksPclRgbFloatIntoCanonicalOrder) {
  // PCL "rgb" float: bits 0x00RRGGBB -> little-endian memory [B, G, R, 0].
  auto cloud = makePackedColorCloud("rgb", DT::kFloat32, {30, 20, 10, 0});  // B=30, G=20, R=10
  EXPECT_TRUE(repackPclPackedColor(cloud));

  ASSERT_EQ(cloud.fields.size(), 4u);
  EXPECT_EQ(cloud.fields[3].name, "rgba");
  EXPECT_EQ(cloud.fields[3].datatype, DT::kUint32);
  EXPECT_EQ(cloud.fields[3].offset, 12u);
  // Canonical R,G,B,A in increasing address; alpha synthesized opaque.
  EXPECT_EQ(cloud.data.data()[12], 10);   // R
  EXPECT_EQ(cloud.data.data()[13], 20);   // G
  EXPECT_EQ(cloud.data.data()[14], 30);   // B
  EXPECT_EQ(cloud.data.data()[15], 255);  // A
}

TEST(PointCloudColor, RepacksPclRgbaPreservingAlpha) {
  auto cloud = makePackedColorCloud("rgba", DT::kUint32, {30, 20, 10, 128});  // B,G,R,A
  EXPECT_TRUE(repackPclPackedColor(cloud));
  EXPECT_EQ(cloud.data.data()[12], 10);   // R
  EXPECT_EQ(cloud.data.data()[14], 30);   // B
  EXPECT_EQ(cloud.data.data()[15], 128);  // A preserved
}

TEST(PointCloudColor, RepackLeavesBigEndianAndPlainClouds) {
  auto big_endian = makePackedColorCloud("rgb", DT::kFloat32, {30, 20, 10, 0});
  big_endian.is_bigendian = true;
  EXPECT_FALSE(repackPclPackedColor(big_endian));

  auto plain = makeCloud(16, {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}});
  EXPECT_FALSE(repackPclPackedColor(plain));
}

TEST(PointCloudColor, NormalizeHandlesSeparateChannelsAndPacked) {
  auto separate = makeCloud(
      20, {{"x", 0, DT::kFloat32, 1},
           {"red", 16, DT::kUint8, 1},
           {"green", 17, DT::kUint8, 1},
           {"blue", 18, DT::kUint8, 1},
           {"alpha", 19, DT::kUint8, 1}});
  EXPECT_TRUE(normalizeCanonicalColor(separate));
  EXPECT_EQ(separate.fields.back().name, "rgba");

  auto packed = makePackedColorCloud("rgb", DT::kFloat32, {30, 20, 10, 0});
  EXPECT_TRUE(normalizeCanonicalColor(packed));
  EXPECT_EQ(packed.fields.back().name, "rgba");
  EXPECT_EQ(packed.fields.back().datatype, DT::kUint32);
}

}  // namespace
