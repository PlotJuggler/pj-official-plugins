// SPDX-License-Identifier: MIT
#include "las_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct TestPoint {
  std::array<int32_t, 3> xyz;
  uint16_t intensity = 0;
  uint8_t return_number = 1;
  uint8_t number_of_returns = 1;
  uint8_t classification = 0;
  uint8_t flags = 0;       // synthetic/key-point/withheld(/overlap)
  int16_t scan_angle = 0;  // degrees (formats 0-5) or 0.006 degree units (6-10)
  uint8_t user_data = 0;
  uint16_t point_source_id = 0;
  double gps_time = 0.0;
  std::array<uint16_t, 4> rgbn{0, 0, 0, 0};
};

template <typename T>
void putAt(std::vector<uint8_t>& b, size_t off, T v) {
  std::memcpy(b.data() + off, &v, sizeof(T));
}

constexpr std::array<uint16_t, 11> kRecordLength = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};

// Build a LAS file with the given version, point format and points. `extra_bytes`
// pads each record past the standard length; `vlr_bytes` sits between the header
// and the point data.
std::vector<uint8_t> makeLas(
    uint8_t minor, uint8_t format, const std::vector<TestPoint>& pts, std::array<double, 3> scale,
    std::array<double, 3> offset, uint16_t extra_bytes = 0, uint32_t vlr_bytes = 0) {
  const uint16_t header_size = minor >= 4 ? 375 : (minor == 3 ? 235 : 227);
  const uint16_t record_length = static_cast<uint16_t>(kRecordLength[format] + extra_bytes);
  const uint32_t data_offset = header_size + vlr_bytes;
  std::vector<uint8_t> b(data_offset + pts.size() * record_length, 0);
  std::memcpy(b.data(), "LASF", 4);
  b[24] = 1;
  b[25] = minor;
  putAt<uint16_t>(b, 94, header_size);
  putAt<uint32_t>(b, 96, data_offset);
  b[104] = format;
  putAt<uint16_t>(b, 105, record_length);
  putAt<uint32_t>(b, 107, minor >= 4 ? 0u : static_cast<uint32_t>(pts.size()));
  for (size_t a = 0; a < 3; ++a) {
    putAt<double>(b, 131 + 8 * a, scale[a]);
    putAt<double>(b, 155 + 8 * a, offset[a]);
  }
  if (minor >= 4) {
    putAt<uint64_t>(b, 247, pts.size());
  }
  for (size_t i = 0; i < pts.size(); ++i) {
    const TestPoint& p = pts[i];
    const size_t r = data_offset + i * record_length;
    for (size_t a = 0; a < 3; ++a) {
      putAt<int32_t>(b, r + 4 * a, p.xyz[a]);
    }
    putAt<uint16_t>(b, r + 12, p.intensity);
    if (format < 6) {
      b[r + 14] = static_cast<uint8_t>(p.return_number | (p.number_of_returns << 3));
      b[r + 15] = static_cast<uint8_t>(p.classification | (p.flags << 5));
      b[r + 16] = static_cast<uint8_t>(static_cast<int8_t>(p.scan_angle));
      b[r + 17] = p.user_data;
      putAt<uint16_t>(b, r + 18, p.point_source_id);
    } else {
      b[r + 14] = static_cast<uint8_t>(p.return_number | (p.number_of_returns << 4));
      b[r + 15] = p.flags;
      b[r + 16] = p.classification;
      b[r + 17] = p.user_data;
      putAt<int16_t>(b, r + 18, p.scan_angle);
      putAt<uint16_t>(b, r + 20, p.point_source_id);
    }
    if (format == 1 || format == 3 || format == 4 || format == 5) {
      putAt<double>(b, r + 20, p.gps_time);
    } else if (format >= 6) {
      putAt<double>(b, r + 22, p.gps_time);
    }
    size_t rgb = 0;
    if (format == 2) {
      rgb = 20;
    } else if (format == 3 || format == 5) {
      rgb = 28;
    } else if (format == 7 || format == 8 || format == 10) {
      rgb = 30;
    }
    if (rgb != 0) {
      const size_t channels = (format == 8 || format == 10) ? 4 : 3;
      for (size_t c = 0; c < channels; ++c) {
        putAt<uint16_t>(b, r + rgb + 2 * c, p.rgbn[c]);
      }
    }
  }
  return b;
}

PJ::Span<const uint8_t> asSpan(const std::vector<uint8_t>& b) {
  return PJ::Span<const uint8_t>(b.data(), b.size());
}

const PJ::sdk::PointField* findField(const PJ::sdk::PointCloud& c, const std::string& name) {
  for (const auto& f : c.fields) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

template <typename T>
T get(const PJ::sdk::PointCloud& c, uint32_t point, const std::string& name) {
  const auto* f = findField(c, name);
  EXPECT_NE(f, nullptr) << name;
  T v{};
  if (f != nullptr) {
    std::memcpy(&v, c.data.data() + static_cast<size_t>(point) * c.point_step + f->offset, sizeof(T));
  }
  return v;
}

TEST(LasReader, Format0LegacyFields) {
  TestPoint p{};
  p.xyz = {1000, -2000, 300};
  p.intensity = 512;
  p.return_number = 2;
  p.number_of_returns = 3;
  p.classification = 6;
  p.flags = 0b101;  // synthetic + withheld
  p.scan_angle = -12;
  p.user_data = 7;
  p.point_source_id = 42;
  auto bytes = makeLas(2, 0, {p}, {0.01, 0.01, 0.01}, {10.0, 20.0, 30.0});
  auto r = pj3d::readLas(asSpan(bytes), "scan");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  const auto& c = r->cloud;
  EXPECT_EQ(c.frame_id, "scan");
  EXPECT_EQ(c.width, 1u);
  EXPECT_EQ(c.height, 1u);
  EXPECT_EQ(r->point_format, 0u);
  EXPECT_FLOAT_EQ(get<float>(c, 0, "x"), 20.0f);
  EXPECT_FLOAT_EQ(get<float>(c, 0, "y"), 0.0f);
  EXPECT_FLOAT_EQ(get<float>(c, 0, "z"), 33.0f);
  EXPECT_EQ(get<uint16_t>(c, 0, "intensity"), 512u);
  EXPECT_EQ(get<uint8_t>(c, 0, "return_number"), 2u);
  EXPECT_EQ(get<uint8_t>(c, 0, "number_of_returns"), 3u);
  EXPECT_EQ(get<uint8_t>(c, 0, "classification"), 6u);
  EXPECT_EQ(get<uint8_t>(c, 0, "classification_flags"), 0b101u);
  EXPECT_FLOAT_EQ(get<float>(c, 0, "scan_angle"), -12.0f);
  EXPECT_EQ(get<uint8_t>(c, 0, "user_data"), 7u);
  EXPECT_EQ(get<uint16_t>(c, 0, "point_source_id"), 42u);
  EXPECT_EQ(findField(c, "gps_time"), nullptr);
  EXPECT_EQ(findField(c, "red"), nullptr);
  EXPECT_EQ(r->origin, (std::array<double, 3>{0.0, 0.0, 0.0}));
}

TEST(LasReader, Format3GpsAndEightBitRgb) {
  TestPoint p{};
  p.xyz = {1, 2, 3};
  p.gps_time = 123456.75;
  p.rgbn = {255, 128, 0, 0};
  auto bytes = makeLas(2, 3, {p}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  EXPECT_DOUBLE_EQ(get<double>(r->cloud, 0, "gps_time"), 123456.75);
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "red"), 255u);
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "green"), 128u);
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "blue"), 0u);
}

TEST(LasReader, SixteenBitRgbIsNarrowed) {
  TestPoint a{};
  a.rgbn = {65535, 32768, 256, 0};
  TestPoint b{};
  b.rgbn = {255, 0, 0, 0};
  auto bytes = makeLas(2, 2, {a, b}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "red"), 255u);
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "green"), 128u);
  EXPECT_EQ(get<uint8_t>(r->cloud, 0, "blue"), 1u);
  EXPECT_EQ(get<uint8_t>(r->cloud, 1, "red"), 0u);  // 255 >> 8
}

TEST(LasReader, Format8Las14) {
  TestPoint p{};
  p.xyz = {5, 6, 7};
  p.return_number = 9;
  p.number_of_returns = 12;
  p.classification = 200;
  p.flags = 0b1000;       // overlap
  p.scan_angle = -15000;  // -90 degrees
  p.user_data = 3;
  p.point_source_id = 9000;
  p.gps_time = 1.5;
  p.rgbn = {1000, 2000, 3000, 4000};
  auto bytes = makeLas(4, 8, {p, p}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0}, /*extra_bytes=*/5, /*vlr_bytes=*/54);
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  const auto& c = r->cloud;
  EXPECT_EQ(c.width, 2u);
  EXPECT_EQ(r->version_minor, 4u);
  EXPECT_FLOAT_EQ(get<float>(c, 1, "z"), 7.0f);
  EXPECT_EQ(get<uint8_t>(c, 1, "return_number"), 9u);
  EXPECT_EQ(get<uint8_t>(c, 1, "number_of_returns"), 12u);
  EXPECT_EQ(get<uint8_t>(c, 1, "classification"), 200u);
  EXPECT_EQ(get<uint8_t>(c, 1, "classification_flags"), 0b1000u);
  EXPECT_NEAR(get<float>(c, 1, "scan_angle"), -90.0f, 1e-3f);
  EXPECT_EQ(get<uint16_t>(c, 1, "point_source_id"), 9000u);
  EXPECT_DOUBLE_EQ(get<double>(c, 1, "gps_time"), 1.5);
  EXPECT_EQ(get<uint8_t>(c, 1, "red"), 1000u >> 8);
  EXPECT_EQ(get<uint16_t>(c, 1, "nir"), 4000u);
}

TEST(LasReader, LargeCoordinatesAreRecentred) {
  TestPoint a{};
  a.xyz = {50012345, 412345678, 1234};  // mm: x=500123.45 m, y=4123456.78 m, z=12.34 m
  TestPoint b{};
  b.xyz = {50012445, 412345778, 1334};
  auto bytes = makeLas(2, 0, {a, b}, {0.01, 0.01, 0.01}, {0.0, 0.0, 0.0});
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  EXPECT_DOUBLE_EQ(r->origin[0], 500123.0);
  EXPECT_DOUBLE_EQ(r->origin[1], 4123456.0);
  EXPECT_DOUBLE_EQ(r->origin[2], 0.0);  // z stays below the threshold
  EXPECT_NEAR(get<float>(r->cloud, 0, "x"), 0.45f, 1e-4f);
  EXPECT_NEAR(get<float>(r->cloud, 1, "y"), 1.78f, 1e-4f);
  EXPECT_NEAR(get<float>(r->cloud, 1, "z"), 13.34f, 1e-4f);
}

TEST(LasReader, EmptyCloud) {
  auto bytes = makeLas(2, 1, {}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_TRUE(r) << (r ? "" : r.error());
  EXPECT_EQ(r->cloud.width, 0u);
  EXPECT_TRUE(r->cloud.data.empty());
}

TEST(LasReader, RejectsBadSignature) {
  auto bytes = makeLas(2, 0, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  bytes[0] = 'X';
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
  std::vector<uint8_t> tiny = {'L', 'A', 'S', 'F'};
  EXPECT_FALSE(pj3d::readLas(asSpan(tiny), "c"));
}

TEST(LasReader, RejectsLaz) {
  auto bytes = makeLas(2, 3, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  bytes[104] = static_cast<uint8_t>(3 | 0x80);
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("LAZ"), std::string::npos);
}

TEST(LasReader, RejectsTruncatedData) {
  auto bytes = makeLas(2, 0, {TestPoint{}, TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  bytes.resize(bytes.size() - 1);
  auto r = pj3d::readLas(asSpan(bytes), "c");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().find("truncated"), std::string::npos);
}

TEST(LasReader, RejectsShortRecordAndBadFormat) {
  auto bytes = makeLas(2, 1, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  putAt<uint16_t>(bytes, 105, 20);  // format 1 needs 28
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
  bytes = makeLas(2, 0, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  bytes[104] = 11;
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
}

TEST(LasReader, RejectsZeroScaleAndBadOffsets) {
  auto bytes = makeLas(2, 0, {TestPoint{}}, {0.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
  bytes = makeLas(2, 0, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  putAt<uint32_t>(bytes, 96, 10);  // point data inside the header
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
  putAt<uint32_t>(bytes, 96, 0xFFFFFFFFu);
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
}

TEST(LasReader, RejectsUnsupportedVersion) {
  auto bytes = makeLas(2, 0, {TestPoint{}}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  bytes[24] = 2;
  EXPECT_FALSE(pj3d::readLas(asSpan(bytes), "c"));
}

}  // namespace
