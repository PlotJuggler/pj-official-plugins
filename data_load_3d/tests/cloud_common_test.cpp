// SPDX-License-Identifier: MIT
#include "cloud_common.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <pj_base/builtin/point_cloud_codec.hpp>

namespace {

using pj3d::DataFormat;
using pj3d::Datatype;
using pj3d::ParsedField;

std::vector<ParsedField> xyziFields() {
  return {
      {"x", Datatype::kFloat32, 1},
      {"y", Datatype::kFloat32, 1},
      {"z", Datatype::kFloat32, 1},
      {"intensity", Datatype::kFloat32, 1}};
}

void putF32LE(std::vector<uint8_t>& out, float v) {
  uint8_t b[4];
  std::memcpy(b, &v, 4);
  out.insert(out.end(), b, b + 4);
}

TEST(CloudCommon, LayoutOffsetsAndStride) {
  auto layout = pj3d::computeLayout(xyziFields());
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->point_step, 16u);
  ASSERT_EQ(layout->fields.size(), 4u);
  EXPECT_EQ(layout->fields[0].offset, 0u);
  EXPECT_EQ(layout->fields[1].offset, 4u);
  EXPECT_EQ(layout->fields[2].offset, 8u);
  EXPECT_EQ(layout->fields[3].offset, 12u);
}

TEST(CloudCommon, BuildFromBinaryLittleEndian) {
  std::vector<uint8_t> body;
  putF32LE(body, 1.0f);
  putF32LE(body, 2.0f);
  putF32LE(body, 3.0f);
  putF32LE(body, 100.0f);
  putF32LE(body, 4.0f);
  putF32LE(body, 5.0f);
  putF32LE(body, 6.0f);
  putF32LE(body, 200.0f);

  auto built = pj3d::buildPointCloud(
      xyziFields(), 2, 1, true, "test", DataFormat::kBinaryLittleEndian,
      PJ::Span<const uint8_t>(body.data(), body.size()));
  ASSERT_TRUE(built);
  EXPECT_EQ(built->width, 2u);
  EXPECT_EQ(built->point_step, 16u);
  EXPECT_FALSE(built->is_bigendian);

  // Round-trip through the canonical codec, then read point 1's intensity.
  auto bytes = PJ::serializePointCloud(*built);
  auto pc = PJ::deserializePointCloud(bytes.data(), bytes.size());
  ASSERT_TRUE(pc);
  float intensity1 = 0.0f;
  std::memcpy(&intensity1, pc->data.data() + 1u * 16u + 12u, 4);
  EXPECT_FLOAT_EQ(intensity1, 200.0f);
}

TEST(CloudCommon, BuildFromAsciiMatchesBinary) {
  const char* text = "1 2 3 100\n4 5 6 200\n";
  PJ::Span<const uint8_t> body(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  auto built = pj3d::buildPointCloud(xyziFields(), 2, 1, true, "test", DataFormat::kAscii, body);
  ASSERT_TRUE(built);
  float x1 = 0.0f, z0 = 0.0f;
  std::memcpy(&x1, built->data.data() + 16u + 0u, 4);
  std::memcpy(&z0, built->data.data() + 8u, 4);
  EXPECT_FLOAT_EQ(x1, 4.0f);
  EXPECT_FLOAT_EQ(z0, 3.0f);
}

TEST(CloudCommon, BuildFromBinaryBigEndianByteSwaps) {
  // One point, x=1.0f. Big-endian bytes of 1.0f = 3F 80 00 00.
  std::vector<uint8_t> body = {0x3F, 0x80, 0x00, 0x00,   // x
                               0x40, 0x00, 0x00, 0x00,   // y = 2.0f BE
                               0x40, 0x40, 0x00, 0x00,   // z = 3.0f BE
                               0x42, 0xC8, 0x00, 0x00};  // intensity = 100.0f BE
  auto built = pj3d::buildPointCloud(
      xyziFields(), 1, 1, true, "t", DataFormat::kBinaryBigEndian, PJ::Span<const uint8_t>(body.data(), body.size()));
  ASSERT_TRUE(built);
  float x = 0.0f;
  std::memcpy(&x, built->data.data() + 0u, 4);
  EXPECT_FLOAT_EQ(x, 1.0f);
}

TEST(CloudCommon, Centroid) {
  std::vector<uint8_t> body;
  putF32LE(body, 0.0f);
  putF32LE(body, 0.0f);
  putF32LE(body, 0.0f);
  putF32LE(body, 0.0f);
  putF32LE(body, 2.0f);
  putF32LE(body, 4.0f);
  putF32LE(body, 6.0f);
  putF32LE(body, 0.0f);
  auto built = pj3d::buildPointCloud(
      xyziFields(), 2, 1, true, "t", DataFormat::kBinaryLittleEndian,
      PJ::Span<const uint8_t>(body.data(), body.size()));
  ASSERT_TRUE(built);
  auto c = pj3d::computeCentroid(*built);
  ASSERT_TRUE(c.has_value());
  EXPECT_DOUBLE_EQ((*c)[0], 1.0);
  EXPECT_DOUBLE_EQ((*c)[1], 2.0);
  EXPECT_DOUBLE_EQ((*c)[2], 3.0);
}

TEST(CloudCommon, SizeMismatchIsError) {
  std::vector<uint8_t> body(10);  // not a multiple of 16
  auto built = pj3d::buildPointCloud(
      xyziFields(), 2, 1, true, "t", DataFormat::kBinaryLittleEndian,
      PJ::Span<const uint8_t>(body.data(), body.size()));
  EXPECT_FALSE(built);
}

TEST(CloudCommon, OverflowingDimensionsAreRejected) {
  // width*height*point_step vastly exceeds kMaxCloudBytes (and would overflow
  // a 32-bit byte count outright). Must be rejected before any allocation.
  auto built = pj3d::buildPointCloud(
      xyziFields(), 0xFFFFFFFFu, 0xFFFFFFFFu, true, "t", DataFormat::kBinaryLittleEndian, PJ::Span<const uint8_t>());
  EXPECT_FALSE(built);
}

TEST(CloudCommon, ComputeLayoutUnknownDatatypeIsError) {
  std::vector<ParsedField> fields = {{"x", Datatype::kFloat32, 1}, {"bad", Datatype::kUnknown, 1}};
  auto layout = pj3d::computeLayout(fields);
  EXPECT_FALSE(layout);
}

TEST(CloudCommon, AsciiNotEnoughTokensIsError) {
  const char* text = "1 2 3\n";  // missing the 4th (intensity) token
  PJ::Span<const uint8_t> body(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  auto built = pj3d::buildPointCloud(xyziFields(), 1, 1, true, "t", DataFormat::kAscii, body);
  EXPECT_FALSE(built);
}

std::vector<ParsedField> countThreeFields() {
  return {{"vec", Datatype::kFloat32, 3}};
}

TEST(CloudCommon, CountFieldBinaryBigEndianByteSwapsEachElement) {
  // One point, a single field with count=3: vec = [1.0f, 2.0f, 3.0f], each
  // encoded big-endian in the body.
  std::vector<uint8_t> body = {0x3F, 0x80, 0x00, 0x00,   // 1.0f BE
                               0x40, 0x00, 0x00, 0x00,   // 2.0f BE
                               0x40, 0x40, 0x00, 0x00};  // 3.0f BE
  auto built = pj3d::buildPointCloud(
      countThreeFields(), 1, 1, true, "t", DataFormat::kBinaryBigEndian,
      PJ::Span<const uint8_t>(body.data(), body.size()));
  ASSERT_TRUE(built);
  float v0 = 0.0f, v1 = 0.0f, v2 = 0.0f;
  std::memcpy(&v0, built->data.data() + 0u, 4);
  std::memcpy(&v1, built->data.data() + 4u, 4);
  std::memcpy(&v2, built->data.data() + 8u, 4);
  EXPECT_FLOAT_EQ(v0, 1.0f);
  EXPECT_FLOAT_EQ(v1, 2.0f);
  EXPECT_FLOAT_EQ(v2, 3.0f);
}

TEST(CloudCommon, CountFieldAsciiPacksEachElement) {
  const char* text = "1 2 3\n";
  PJ::Span<const uint8_t> body(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  auto built = pj3d::buildPointCloud(countThreeFields(), 1, 1, true, "t", DataFormat::kAscii, body);
  ASSERT_TRUE(built);
  float v0 = 0.0f, v1 = 0.0f, v2 = 0.0f;
  std::memcpy(&v0, built->data.data() + 0u, 4);
  std::memcpy(&v1, built->data.data() + 4u, 4);
  std::memcpy(&v2, built->data.data() + 8u, 4);
  EXPECT_FLOAT_EQ(v0, 1.0f);
  EXPECT_FLOAT_EQ(v1, 2.0f);
  EXPECT_FLOAT_EQ(v2, 3.0f);
}

TEST(CloudCommon, CopySurvivesOriginalExpectedDestruction) {
  // Proves the BufferAnchor pattern: build inside a scope, copy the
  // PointCloud out, let the original Expected<PointCloud> (and its shared_ptr
  // reference) be destroyed, then verify the copy's data span still reads
  // correctly.
  PJ::sdk::PointCloud copy;
  {
    std::vector<uint8_t> body;
    putF32LE(body, 1.0f);
    putF32LE(body, 2.0f);
    putF32LE(body, 3.0f);
    putF32LE(body, 100.0f);
    auto built = pj3d::buildPointCloud(
        xyziFields(), 1, 1, true, "t", DataFormat::kBinaryLittleEndian,
        PJ::Span<const uint8_t>(body.data(), body.size()));
    ASSERT_TRUE(built);
    copy = *built;  // shares ownership of the backing buffer via `anchor`
  }  // `built` destroyed here; `copy` must keep the bytes alive
  ASSERT_EQ(copy.data.size(), 16u);
  float x = 0.0f, intensity = 0.0f;
  std::memcpy(&x, copy.data.data() + 0u, 4);
  std::memcpy(&intensity, copy.data.data() + 12u, 4);
  EXPECT_FLOAT_EQ(x, 1.0f);
  EXPECT_FLOAT_EQ(intensity, 100.0f);
}

TEST(CloudCommon, PointsWithNoFieldsIsError) {
  // Zero fields => point_step 0. A positive point count is malformed and must
  // be rejected, not spin the ascii fill loop num_points times.
  std::vector<ParsedField> no_fields;
  const char* text = "1 2 3\n";
  PJ::Span<const uint8_t> body(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  auto built = pj3d::buildPointCloud(no_fields, 1000000, 1000000, true, "t", DataFormat::kAscii, body);
  EXPECT_FALSE(built);
}

}  // namespace
