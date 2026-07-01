#include "../mf4_value.hpp"

#include <gtest/gtest.h>
#include <mdf/ichannel.h>

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
