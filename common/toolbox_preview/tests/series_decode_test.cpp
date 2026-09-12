// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the type gate and the Arrow value-column decoder. Both are pure, so
// these run against stack arrays with no toolbox host involved.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <toolbox_preview/series_types.hpp>
#include <vector>

namespace {

using PJ::PrimitiveType;
using toolbox_preview::decodeSeriesAsDouble;
using toolbox_preview::isPlottableType;

// Timestamps are irrelevant to the value decoding; a simple ramp keeps the assertions
// readable and makes a dropped sample obvious.
std::vector<std::int64_t> ramp(std::size_t n) {
  std::vector<std::int64_t> ts(n);
  for (std::size_t i = 0; i < n; ++i) {
    ts[i] = static_cast<std::int64_t>(i) * 1000;
  }
  return ts;
}

// ---------------------------------------------------------------------------
// isPlottableType — the gate that decides what the source list may offer
// ---------------------------------------------------------------------------

TEST(IsPlottableType, EveryNumericTypeIsPlottable) {
  EXPECT_TRUE(isPlottableType(PrimitiveType::kFloat32));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kFloat64));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kInt8));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kInt16));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kInt32));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kInt64));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kUint8));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kUint16));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kUint32));
  EXPECT_TRUE(isPlottableType(PrimitiveType::kUint64));
}

// Named for the reason, not the value: toolbox_fft's isNumericType excludes bool on
// purpose, and someone "harmonising" the two predicates would silently delete the
// Boolean flag detector from the UI.
TEST(IsPlottableType, BoolIsPlottableBecauseTheBooleanFlagBuiltinNeedsIt) {
  EXPECT_TRUE(isPlottableType(PrimitiveType::kBool));
}

TEST(IsPlottableType, StringAndUnspecifiedAreRejected) {
  // These are exactly the two the host's marker-series resolver refuses, which is why a
  // rule aimed at them dies with "attempt to index nil value".
  EXPECT_FALSE(isPlottableType(PrimitiveType::kString));
  EXPECT_FALSE(isPlottableType(PrimitiveType::kUnspecified));
}

TEST(IsPlottableType, IsUsableInAConstantExpression) {
  static_assert(isPlottableType(PrimitiveType::kFloat64));
  static_assert(!isPlottableType(PrimitiveType::kString));
  SUCCEED();
}

// ---------------------------------------------------------------------------
// decodeSeriesAsDouble — numeric columns
// ---------------------------------------------------------------------------

TEST(DecodeSeries, Float64RoundTripsExactly) {
  const std::vector<double> values{-1.5, 0.0, 2.25};
  const auto ts = ramp(values.size());
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(
      PrimitiveType::kFloat64, values.data(), nullptr, 0, ts.data(), values.size(), out_ts, out_values));
  EXPECT_EQ(out_values, values);
  EXPECT_EQ(out_ts, (std::vector<double>{0.0, 1000.0, 2000.0}));
}

TEST(DecodeSeries, Float32WidensExactlyForRepresentableValuesAndNotOtherwise) {
  const std::vector<float> values{0.5F, 0.1F};
  const auto ts = ramp(values.size());
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(
      PrimitiveType::kFloat32, values.data(), nullptr, 0, ts.data(), values.size(), out_ts, out_values));
  // 0.5 is exact in both widths; 0.1 is not, and widening preserves the float32 error
  // rather than inventing the double value of the literal.
  EXPECT_DOUBLE_EQ(out_values[0], 0.5);
  EXPECT_NE(out_values[1], 0.1);
  EXPECT_DOUBLE_EQ(out_values[1], static_cast<double>(0.1F));
}

TEST(DecodeSeries, SignedAndUnsignedBoundariesSurvive) {
  const std::vector<std::int8_t> i8{std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
  const std::vector<std::uint8_t> u8{0U, std::numeric_limits<std::uint8_t>::max()};
  const auto ts = ramp(2);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kInt8, i8.data(), nullptr, 0, ts.data(), 2, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{-128.0, 127.0}));

  // Fresh outputs rather than clearing: reusing them would let a failure above leak into
  // the assertion below.
  std::vector<double> u8_ts;
  std::vector<double> u8_values;
  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kUint8, u8.data(), nullptr, 0, ts.data(), 2, u8_ts, u8_values));
  EXPECT_EQ(u8_values, (std::vector<double>{0.0, 255.0}));
}

TEST(DecodeSeries, Uint64BeyondDoublePrecisionLosesLowBitsAsExpected) {
  // Not a bug to fix — a documented consequence of a double-valued preview. Asserted so
  // the loss stays deliberate instead of surprising someone later.
  const std::vector<std::uint64_t> values{(1ULL << 60) + 7ULL, std::numeric_limits<std::uint64_t>::max()};
  const auto ts = ramp(values.size());
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(
      PrimitiveType::kUint64, values.data(), nullptr, 0, ts.data(), values.size(), out_ts, out_values));
  EXPECT_DOUBLE_EQ(out_values[0], static_cast<double>((1ULL << 60) + 7ULL));
  EXPECT_NE(static_cast<std::uint64_t>(out_values[0]), (1ULL << 60) + 7ULL);
  EXPECT_DOUBLE_EQ(out_values[1], 18446744073709551616.0);
}

TEST(DecodeSeries, Int64NegativeExtremeSurvives) {
  const std::vector<std::int64_t> values{-(1LL << 62)};
  const auto ts = ramp(1);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kInt64, values.data(), nullptr, 0, ts.data(), 1, out_ts, out_values));
  EXPECT_DOUBLE_EQ(out_values[0], static_cast<double>(-(1LL << 62)));
}

// ---------------------------------------------------------------------------
// decodeSeriesAsDouble — bool, the packed-bitmap column
// ---------------------------------------------------------------------------

TEST(DecodeSeries, BoolReadsAcrossAByteBoundary) {
  // 10 samples => the read must continue into byte 1. A decoder that casts the buffer to
  // `const bool*` passes any test of <= 8 samples and fails here.
  // bits (LSB first): 1,0,1,1,0,0,0,0 | 1,0
  const std::vector<std::uint8_t> bits{0b00001101U, 0b00000001U};
  const auto ts = ramp(10);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kBool, bits.data(), nullptr, 0, ts.data(), 10, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{1.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0}));
}

TEST(DecodeSeries, BoolAllSetAndAllClear) {
  const std::vector<std::uint8_t> ones{0xFFU};
  const std::vector<std::uint8_t> zeros{0x00U};
  const auto ts = ramp(8);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kBool, ones.data(), nullptr, 0, ts.data(), 8, out_ts, out_values));
  EXPECT_EQ(out_values, std::vector<double>(8, 1.0));

  std::vector<double> zero_ts;
  std::vector<double> zero_values;
  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kBool, zeros.data(), nullptr, 0, ts.data(), 8, zero_ts, zero_values));
  EXPECT_EQ(zero_values, std::vector<double>(8, 0.0));
}

TEST(DecodeSeries, BoolHonoursABitOffsetNotAByteOffset) {
  // Same buffer as the byte-boundary test, sliced from sample 3: the offset shifts which
  // bit within the byte each sample lives at, which is what makes bool different.
  const std::vector<std::uint8_t> bits{0b00001101U, 0b00000001U};
  const auto ts = ramp(4);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kBool, bits.data(), nullptr, 3, ts.data(), 4, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{1.0, 0.0, 0.0, 0.0}));
}

TEST(DecodeSeries, NumericHonoursAnElementOffset) {
  const std::vector<std::int32_t> values{10, 20, 30, 40, 50};
  const auto ts = ramp(3);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(PrimitiveType::kInt32, values.data(), nullptr, 2, ts.data(), 3, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{30.0, 40.0, 50.0}));
}

// ---------------------------------------------------------------------------
// decodeSeriesAsDouble — the validity bitmap
// ---------------------------------------------------------------------------

TEST(DecodeSeries, NullSampleIsDroppedTogetherWithItsTimestamp) {
  const std::vector<double> values{1.0, 999.0, 3.0};
  const auto ts = ramp(3);
  const std::vector<std::uint8_t> validity{0b00000101U};  // sample 1 is null
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(
      PrimitiveType::kFloat64, values.data(), validity.data(), 0, ts.data(), 3, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{1.0, 3.0}));
  EXPECT_EQ(out_ts, (std::vector<double>{0.0, 2000.0}));
  EXPECT_EQ(out_ts.size(), out_values.size());
}

TEST(DecodeSeries, NullFirstSampleMovesT0) {
  // t0 is the preview's X origin (SeriesCatalog::t0 reads timestamps.front()), so a
  // leading null must not anchor the chart at a sample that does not exist.
  const std::vector<double> values{999.0, 2.0, 3.0};
  const auto ts = ramp(3);
  const std::vector<std::uint8_t> validity{0b00000110U};  // sample 0 is null
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(decodeSeriesAsDouble(
      PrimitiveType::kFloat64, values.data(), validity.data(), 0, ts.data(), 3, out_ts, out_values));
  ASSERT_FALSE(out_ts.empty());
  EXPECT_DOUBLE_EQ(out_ts.front(), 1000.0);
}

TEST(DecodeSeries, NullBoolWouldOtherwiseForgeAnEdge) {
  // The reason nulls are dropped rather than read as zero: a null bool leaves its bit at
  // 0, and "Boolean flag (edges)" detects edges with `v ~= 0`, so a gap inside a raised
  // flag would emit a phantom falling AND rising edge.
  const std::vector<std::uint8_t> bits{0b00000101U};  // 1,0,1 — the middle 0 is a NULL
  const auto ts = ramp(3);
  const std::vector<std::uint8_t> validity{0b00000101U};
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(
      decodeSeriesAsDouble(PrimitiveType::kBool, bits.data(), validity.data(), 0, ts.data(), 3, out_ts, out_values));
  EXPECT_EQ(out_values, (std::vector<double>{1.0, 1.0}));  // flag stays raised, no edge
}

TEST(DecodeSeries, NullValidityPointerMeansEverySampleIsValid) {
  const std::vector<double> values{1.0, 2.0};
  const auto ts = ramp(2);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  ASSERT_TRUE(
      decodeSeriesAsDouble(PrimitiveType::kFloat64, values.data(), nullptr, 0, ts.data(), 2, out_ts, out_values));
  EXPECT_EQ(out_values.size(), 2U);
}

// ---------------------------------------------------------------------------
// decodeSeriesAsDouble — rejections
// ---------------------------------------------------------------------------

TEST(DecodeSeries, NonNumericTypesAreRejectedWithoutWriting) {
  const std::vector<double> values{1.0};
  const auto ts = ramp(1);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  EXPECT_FALSE(
      decodeSeriesAsDouble(PrimitiveType::kString, values.data(), nullptr, 0, ts.data(), 1, out_ts, out_values));
  EXPECT_FALSE(
      decodeSeriesAsDouble(PrimitiveType::kUnspecified, values.data(), nullptr, 0, ts.data(), 1, out_ts, out_values));
  EXPECT_TRUE(out_values.empty());
  EXPECT_TRUE(out_ts.empty());
}

TEST(DecodeSeries, NullBuffersAreRejected) {
  const auto ts = ramp(1);
  std::vector<double> out_ts;
  std::vector<double> out_values;

  EXPECT_FALSE(decodeSeriesAsDouble(PrimitiveType::kFloat64, nullptr, nullptr, 0, ts.data(), 1, out_ts, out_values));
  const std::vector<double> values{1.0};
  EXPECT_FALSE(
      decodeSeriesAsDouble(PrimitiveType::kFloat64, values.data(), nullptr, 0, nullptr, 1, out_ts, out_values));
}

}  // namespace
