// SPDX-License-Identifier: MIT
#include "pcd_reader.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

PJ::Span<const uint8_t> asSpan(const std::string& s) {
  return PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void putF32LE(std::string& s, float v) {
  char b[4];
  std::memcpy(b, &v, 4);
  s.append(b, 4);
}

TEST(PcdReader, AsciiXyzIntensity) {
  std::string pcd =
      "# .PCD v0.7\nVERSION 0.7\nFIELDS x y z intensity\nSIZE 4 4 4 4\n"
      "TYPE F F F F\nCOUNT 1 1 1 1\nWIDTH 2\nHEIGHT 1\n"
      "VIEWPOINT 0 0 0 1 0 0 0\nPOINTS 2\nDATA ascii\n"
      "1 2 3 100\n4 5 6 200\n";
  auto built = pj3d::readPcd(asSpan(pcd), "cloud");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  EXPECT_EQ(built->width, 2u);
  EXPECT_EQ(built->point_step, 16u);
  ASSERT_EQ(built->fields.size(), 4u);
  EXPECT_EQ(built->fields[3].name, "intensity");
  EXPECT_EQ(built->frame_id, "cloud");
  float y1 = 0.0f;
  std::memcpy(&y1, built->data.data() + 16u + 4u, 4);
  EXPECT_FLOAT_EQ(y1, 5.0f);
}

TEST(PcdReader, BinaryMatchesAscii) {
  std::string header =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      "WIDTH 2\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 2\nDATA binary\n";
  std::string pcd = header;
  putF32LE(pcd, 1.0f);
  putF32LE(pcd, 2.0f);
  putF32LE(pcd, 3.0f);
  putF32LE(pcd, 4.0f);
  putF32LE(pcd, 5.0f);
  putF32LE(pcd, 6.0f);
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  float z1 = 0.0f;
  std::memcpy(&z1, built->data.data() + 12u + 8u, 4);
  EXPECT_FLOAT_EQ(z1, 6.0f);
}

TEST(PcdReader, OrganizedKeepsWidthHeight) {
  std::string header =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      "WIDTH 2\nHEIGHT 2\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 4\nDATA binary\n";
  std::string pcd = header;
  for (int i = 0; i < 4; ++i) {
    putF32LE(pcd, 0.0f);
    putF32LE(pcd, 0.0f);
    putF32LE(pcd, 0.0f);
  }
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  EXPECT_EQ(built->width, 2u);
  EXPECT_EQ(built->height, 2u);
}

TEST(PcdReader, BinaryCompressedIsCleanErrorForNow) {
  std::string pcd =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary_compressed\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  ASSERT_FALSE(built);
  EXPECT_NE(built.error().find("binary_compressed"), std::string::npos);
}

TEST(PcdReader, RgbFieldMappedAsUint32) {
  std::string header =
      "VERSION 0.7\nFIELDS x y z rgb\nSIZE 4 4 4 4\nTYPE F F F U\nCOUNT 1 1 1 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  std::string pcd = header;
  putF32LE(pcd, 0.0f);
  putF32LE(pcd, 0.0f);
  putF32LE(pcd, 0.0f);
  uint32_t rgb = 0x00FF8040u;
  char b[4];
  std::memcpy(b, &rgb, 4);
  pcd.append(b, 4);
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  ASSERT_EQ(built->fields.size(), 4u);
  EXPECT_EQ(built->fields[3].name, "rgb");
  EXPECT_EQ(built->fields[3].datatype, pj3d::Datatype::kUint32);
}

// Malformed numeric header field must be a clean error, NOT a thrown exception.
TEST(PcdReader, MalformedWidthIsCleanError) {
  std::string pcd =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1 1\n"
      "WIDTH notanumber\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

TEST(PcdReader, AsciiCrlfHeader) {
  // Same as AsciiXyzIntensity but with CRLF line endings — exercises the
  // trailing-\r strip so DATA parses as "ascii" (not "ascii\r") and the body
  // offset lands past the \n.
  std::string pcd =
      "# .PCD v0.7\r\nVERSION 0.7\r\nFIELDS x y z intensity\r\nSIZE 4 4 4 4\r\n"
      "TYPE F F F F\r\nCOUNT 1 1 1 1\r\nWIDTH 2\r\nHEIGHT 1\r\n"
      "VIEWPOINT 0 0 0 1 0 0 0\r\nPOINTS 2\r\nDATA ascii\r\n"
      "1 2 3 100\r\n4 5 6 200\r\n";
  auto built = pj3d::readPcd(asSpan(pcd), "cloud");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  EXPECT_EQ(built->width, 2u);
  EXPECT_EQ(built->point_step, 16u);
  float y1 = 0.0f;
  std::memcpy(&y1, built->data.data() + 16u + 4u, 4);
  EXPECT_FLOAT_EQ(y1, 5.0f);
}

TEST(PcdReader, MalformedSizeIsCleanError) {
  std::string pcd =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 x 4\nTYPE F F F\nCOUNT 1 1 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

TEST(PcdReader, UnsupportedDatatypeIsCleanError) {
  // 8-byte integer (I/8) has no canonical Datatype -> clean error.
  std::string pcd =
      "VERSION 0.7\nFIELDS x y stamp\nSIZE 4 4 8\nTYPE F F I\nCOUNT 1 1 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

TEST(PcdReader, CountArityMismatchIsCleanError) {
  // COUNT present but wrong number of entries -> error (not silent default).
  std::string pcd =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\nCOUNT 1 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

TEST(PcdReader, MultiCharTypeIsCleanError) {
  // "TYPE FLOAT" must NOT be silently accepted as 'F'.
  std::string pcd =
      "VERSION 0.7\nFIELDS x\nSIZE 4\nTYPE FLOAT\nCOUNT 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

TEST(PcdReader, CountAbsentDefaultsToOne) {
  // No COUNT line at all -> every field defaults to count 1 (still valid).
  std::string pcd =
      "VERSION 0.7\nFIELDS x y z\nSIZE 4 4 4\nTYPE F F F\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA binary\n";
  std::string full = pcd;
  putF32LE(full, 7.0f);
  putF32LE(full, 8.0f);
  putF32LE(full, 9.0f);
  auto built = pj3d::readPcd(asSpan(full), "c");
  ASSERT_TRUE(built) << (built ? "" : built.error());
  EXPECT_EQ(built->point_step, 12u);
  ASSERT_EQ(built->fields.size(), 3u);
  EXPECT_EQ(built->fields[0].count, 1u);
}

TEST(PcdReader, UnrecognizedDataModeIsCleanError) {
  std::string pcd =
      "VERSION 0.7\nFIELDS x\nSIZE 4\nTYPE F\nCOUNT 1\n"
      "WIDTH 1\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS 1\nDATA base64\n";
  auto built = pj3d::readPcd(asSpan(pcd), "c");
  EXPECT_FALSE(built);
}

}  // namespace
