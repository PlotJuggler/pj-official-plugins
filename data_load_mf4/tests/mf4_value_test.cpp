#include "../mf4_value.hpp"

#include <gtest/gtest.h>
#include <mdf/ichannel.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <pj_base/type_tree.hpp>

using mdf::ChannelDataType;
using PJ::PrimitiveType;

namespace {

TEST(Mf4Value, NumericTypesMapToFloat64) {
  // All integer/float channels import as CC-applied engineering doubles.
  for (const auto t :
       {ChannelDataType::UnsignedIntegerLe, ChannelDataType::UnsignedIntegerBe, ChannelDataType::SignedIntegerLe,
        ChannelDataType::SignedIntegerBe, ChannelDataType::FloatLe, ChannelDataType::FloatBe}) {
    EXPECT_EQ(mf4_detail::mf4TypeToPrimitive(t), PrimitiveType::kFloat64);
    EXPECT_TRUE(mf4_detail::isSupportedValueType(t));
    EXPECT_FALSE(mf4_detail::isStringType(t));
  }
}

TEST(Mf4Value, StringTypesMapToString) {
  for (const auto t :
       {ChannelDataType::StringAscii, ChannelDataType::StringUTF8, ChannelDataType::StringUTF16Le,
        ChannelDataType::StringUTF16Be}) {
    EXPECT_EQ(mf4_detail::mf4TypeToPrimitive(t), PrimitiveType::kString);
    EXPECT_TRUE(mf4_detail::isSupportedValueType(t));
    EXPECT_TRUE(mf4_detail::isStringType(t));
  }
}

TEST(Mf4Value, UnsupportedTypesMapToUnspecified) {
  // Byte arrays, MIME, CANopen date/time, and complex are out of v1 scope;
  // the reader skips these with a counted warning.
  for (const auto t :
       {ChannelDataType::ByteArray, ChannelDataType::MimeSample, ChannelDataType::MimeStream,
        ChannelDataType::CanOpenDate, ChannelDataType::CanOpenTime, ChannelDataType::ComplexLe,
        ChannelDataType::ComplexBe}) {
    EXPECT_EQ(mf4_detail::mf4TypeToPrimitive(t), PrimitiveType::kUnspecified);
    EXPECT_FALSE(mf4_detail::isSupportedValueType(t));
    EXPECT_FALSE(mf4_detail::isStringType(t));
  }
}

}  // namespace

// File-controlled master times feed llround(t_sec * 1e9) + start_ns; NaN,
// infinity, or out-of-range seconds must be rejected, never converted with UB.
TEST(Mf4Value, RelativeSecondsToNsConvertsNormalValues) {
  const auto ns = mf4_detail::relativeSecondsToNs(1'000, 1.5);
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(*ns, 1'500'000'000 + 1'000);
  const auto neg = mf4_detail::relativeSecondsToNs(0, -2.0);
  ASSERT_TRUE(neg.has_value());
  EXPECT_EQ(*neg, -2'000'000'000);
}

TEST(Mf4Value, RelativeSecondsToNsRejectsNonFiniteAndOutOfRange) {
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(0, std::nan("")), std::nullopt);
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(0, std::numeric_limits<double>::infinity()), std::nullopt);
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(0, -std::numeric_limits<double>::infinity()), std::nullopt);
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(0, 1.0e19), std::nullopt);  // > int64 ns range
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(0, -1.0e19), std::nullopt);
}

TEST(Mf4Value, RelativeSecondsToNsRejectsAdditionOverflow) {
  // A start epoch near INT64_MAX plus a large-but-representable offset.
  EXPECT_EQ(mf4_detail::relativeSecondsToNs(std::numeric_limits<std::int64_t>::max() - 5, 1.0), std::nullopt);
}
