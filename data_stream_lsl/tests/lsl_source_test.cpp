#include <gtest/gtest.h>
#include <lsl_cpp.h>

#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/type_tree.hpp>
#include <variant>

#include "lsl_conversions.hpp"

using pj_lsl::TimestampMode;

TEST(TimestampMode, ParseAndRoundTrip) {
  EXPECT_EQ(pj_lsl::parseTimestampMode("sync"), TimestampMode::kSync);
  EXPECT_EQ(pj_lsl::parseTimestampMode("raw"), TimestampMode::kRaw);
  EXPECT_EQ(pj_lsl::parseTimestampMode("receiver"), TimestampMode::kReceiver);
  EXPECT_EQ(pj_lsl::parseTimestampMode("bogus"), TimestampMode::kSync);  // default

  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kSync), "sync");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kRaw), "raw");
  EXPECT_STREQ(pj_lsl::toString(TimestampMode::kReceiver), "receiver");
}

TEST(ChannelFormat, MapsEveryFormat) {
  using PJ::PrimitiveType;
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_float32), PrimitiveType::kFloat32);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_double64), PrimitiveType::kFloat64);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int8), PrimitiveType::kInt8);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int16), PrimitiveType::kInt16);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int32), PrimitiveType::kInt32);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_int64), PrimitiveType::kInt64);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_string), PrimitiveType::kString);
  EXPECT_EQ(pj_lsl::mapChannelFormat(lsl::cf_undefined), PrimitiveType::kUnspecified);

  EXPECT_TRUE(pj_lsl::isStringFormat(lsl::cf_string));
  EXPECT_FALSE(pj_lsl::isStringFormat(lsl::cf_float32));
}

TEST(ComputeTimestamp, AllModes) {
  const double s = 100.0;                   // remote sample stamp (seconds)
  const double tc = 0.5;                    // time_correction (seconds)
  const int64_t off = 1'000'000'000'000LL;  // local_clock -> epoch offset (ns)
  const int64_t now = 9'999'999'999LL;      // receiver clock (ns)

  // sync: (s + tc) * 1e9 + off
  EXPECT_EQ(
      pj_lsl::computeTimestampNs(TimestampMode::kSync, s, tc, off, now), static_cast<int64_t>((s + tc) * 1e9) + off);
  // sync with s == 0 -> receiver fallback
  EXPECT_EQ(pj_lsl::computeTimestampNs(TimestampMode::kSync, 0.0, tc, off, now), now);
  // raw: s * 1e9 (no offset, no correction)
  EXPECT_EQ(pj_lsl::computeTimestampNs(TimestampMode::kRaw, s, tc, off, now), static_cast<int64_t>(s * 1e9));
  // receiver: now, ignoring the stamp
  EXPECT_EQ(pj_lsl::computeTimestampNs(TimestampMode::kReceiver, s, tc, off, now), now);
}

TEST(NumericValueRef, HoldsNativeType) {
  using PJ::PrimitiveType;
  EXPECT_TRUE(std::holds_alternative<float>(pj_lsl::numericValueRef(PrimitiveType::kFloat32, 1.5)));
  EXPECT_TRUE(std::holds_alternative<double>(pj_lsl::numericValueRef(PrimitiveType::kFloat64, 1.5)));
  EXPECT_TRUE(std::holds_alternative<int8_t>(pj_lsl::numericValueRef(PrimitiveType::kInt8, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int16_t>(pj_lsl::numericValueRef(PrimitiveType::kInt16, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int32_t>(pj_lsl::numericValueRef(PrimitiveType::kInt32, 5.0)));
  EXPECT_TRUE(std::holds_alternative<int64_t>(pj_lsl::numericValueRef(PrimitiveType::kInt64, 5.0)));
  EXPECT_EQ(std::get<int32_t>(pj_lsl::numericValueRef(PrimitiveType::kInt32, 42.0)), 42);
}

TEST(UniqueTopicNames, DisambiguatesCollisions) {
  std::vector<pj_lsl::StreamKey> in = {{"EEG", "amp-01"}, {"EEG", "amp-02"}, {"Markers", ""}, {"Dup", ""}, {"Dup", ""}};
  auto out = pj_lsl::uniqueTopicNames(in);
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out[0], "EEG (amp-01)");
  EXPECT_EQ(out[1], "EEG (amp-02)");
  EXPECT_EQ(out[2], "Markers");  // unique name, unchanged
  EXPECT_EQ(out[3], "Dup #0");   // empty source_id -> numeric suffix
  EXPECT_EQ(out[4], "Dup #1");
}

TEST(ChannelLabels, ReadsXmlWithFallback) {
  // 3-channel float stream; give 2 of 3 channels labels, leave the 3rd blank.
  lsl::stream_info info("TestStream", "EEG", 3, 100.0, lsl::cf_float32, "src-1");
  lsl::xml_element channels = info.desc().append_child("channels");
  channels.append_child("channel").append_child_value("label", "Fp1");
  channels.append_child("channel").append_child_value("label", "Fp2");
  channels.append_child("channel");  // no label -> fallback

  auto labels = pj_lsl::channelLabels(info);
  ASSERT_EQ(labels.size(), 3u);
  EXPECT_EQ(labels[0], "Fp1");
  EXPECT_EQ(labels[1], "Fp2");
  EXPECT_EQ(labels[2], "channel_2");
}

TEST(ChannelLabels, NoDescAllFallback) {
  lsl::stream_info info("Bare", "Misc", 2, 0.0, lsl::cf_double64, "src-2");
  auto labels = pj_lsl::channelLabels(info);
  ASSERT_EQ(labels.size(), 2u);
  EXPECT_EQ(labels[0], "channel_0");
  EXPECT_EQ(labels[1], "channel_1");
}
