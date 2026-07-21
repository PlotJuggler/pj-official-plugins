#include <gtest/gtest.h>

#include <cstdint>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <string>
#include <vector>

namespace {

using mf4_detail::CanDecoder;
using mf4_detail::DecodedSignal;
using mf4_detail::DecodeResult;

// A minimal but complete DBC with:
//  - a standard 8-byte message id 256 (0x100): Speed (LE unsigned, x0.1 km/h),
//    Rpm (LE unsigned), Temp (LE signed 8-bit, degC);
//  - a Vector-flagged extended message 0x80000100 (= id 0x100, extended): ExtSig;
//  - a raw-stored 29-bit id 0x18FEF100 (J1939-style, no Vector flag): J1939Sig;
//  - 32/64-bit unsigned counters (guard the vendored wide-mask fix).
// The two 0x100 messages exercise standard/extended disambiguation.
const char* const kDbc = R"DBC(VERSION "1.0.0"

NS_ :
	BA_
	BA_DEF_
	BA_DEF_DEF_
	BS_
	CM_
	VAL_
	VAL_TABLE_

BS_:

BU_: ECU

BO_ 256 EngineData: 8 ECU
 SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECU
 SG_ Rpm : 16|16@1+ (1,0) [0|65535] "rpm" ECU
 SG_ Temp : 32|8@1- (1,0) [-128|127] "degC" ECU

BO_ 2147483904 ExtMsg: 8 ECU
 SG_ ExtSig : 0|8@1+ (1,0) [0|255] "" ECU

BO_ 419361024 J1939Msg: 8 ECU
 SG_ J1939Sig : 0|8@1+ (1,0) [0|255] "" ECU

BO_ 512 Counters: 8 ECU
 SG_ Count32 : 0|32@1+ (1,0) [0|4294967295] "" ECU

BO_ 513 WideCounter: 8 ECU
 SG_ Count64 : 0|64@1+ (1,0) [0|0] "" ECU
)DBC";

const DecodedSignal* find(const std::vector<DecodedSignal>& sigs, const std::string& name) {
  for (const auto& s : sigs) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

TEST(CanDecoder, DecodesLittleEndianSignals) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 5u);

  // Speed raw 1000 (0x03E8) -> 100.0 km/h; Rpm raw 3000 (0x0BB8) -> 3000 rpm.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0xB8, 0x0B, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, /*extended=*/false, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);

  const auto* speed = find(sigs, "Speed");
  const auto* rpm = find(sigs, "Rpm");
  ASSERT_NE(speed, nullptr);
  ASSERT_NE(rpm, nullptr);
  EXPECT_DOUBLE_EQ(speed->value, 100.0);
  EXPECT_EQ(speed->unit, "km/h");
  EXPECT_DOUBLE_EQ(rpm->value, 3000.0);
  EXPECT_EQ(rpm->unit, "rpm");
}

TEST(CanDecoder, DecodesSignedSignal) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // Temp is a signed 8-bit at byte 4; 0xFF -> -1 degC.
  const std::vector<std::uint8_t> data{0, 0, 0, 0, 0xFF, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, false, data, result);
  const auto* temp = find(sigs, "Temp");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->value, -1.0);
}

TEST(CanDecoder, ExtendedFrameMatchesVectorFlaggedMessage) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // An extended frame with id 0x100 must match the flagged ExtMsg (0x80000100),
  // not the standard EngineData (256) — no collision.
  const std::vector<std::uint8_t> data{5, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x100u, /*extended=*/true, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  const auto* ext = find(sigs, "ExtSig");
  ASSERT_NE(ext, nullptr);
  EXPECT_DOUBLE_EQ(ext->value, 5.0);
  EXPECT_EQ(find(sigs, "Speed"), nullptr);  // did not decode EngineData
}

TEST(CanDecoder, StandardFrameMatchesStandardMessage) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // The same numeric id 0x100 as a standard frame matches EngineData, not ExtMsg.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x100u, false, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  EXPECT_NE(find(sigs, "Speed"), nullptr);
  EXPECT_EQ(find(sigs, "ExtSig"), nullptr);
}

TEST(CanDecoder, ExtendedFrameMatchesRawStored29BitId) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // J1939-style DBCs store 29-bit ids raw (no Vector flag). An extended frame
  // with an id above the 11-bit range must still fall back to the raw entry.
  const std::vector<std::uint8_t> data{7, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x18FEF100u, /*extended=*/true, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  const auto* sig = find(sigs, "J1939Sig");
  ASSERT_NE(sig, nullptr);
  EXPECT_DOUBLE_EQ(sig->value, 7.0);
}

TEST(CanDecoder, ExtendedFrameDoesNotFallBackToStandardRangeId) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // Counters (512) is a standard 11-bit message. An *extended* frame whose id
  // happens to be 512 must NOT silently decode with the standard layout.
  DecodeResult result = DecodeResult::kDecoded;
  const auto sigs = dec.decode(512, /*extended=*/true, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kNoMatch);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, DecodesUnsigned32BitSignal) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // Raw 0x12345678 (LE) -> 305419896. The upstream dbc_parser_cpp masks
  // unsigned values with a 32-bit `1 << size`, which is UB at size >= 32 and
  // decodes every frame as 0 — this guards our vendored fix.
  const std::vector<std::uint8_t> data{0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(512, false, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  const auto* count = find(sigs, "Count32");
  ASSERT_NE(count, nullptr);
  EXPECT_DOUBLE_EQ(count->value, 305419896.0);
}

TEST(CanDecoder, DecodesUnsigned64BitSignal) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // Raw 0x00123456789ABCDE = 5124095576030430 < 2^53, exactly representable.
  const std::vector<std::uint8_t> data{0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x00};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(513, false, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  const auto* count = find(sigs, "Count64");
  ASSERT_NE(count, nullptr);
  EXPECT_DOUBLE_EQ(count->value, 5124095576030430.0);
}

TEST(CanDecoder, TruncatedFrameIsUndecodable) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // EngineData is 8 bytes; a 2-byte frame must not decode to garbage — and the
  // caller must be able to count it (it is not "unmatched").
  const std::vector<std::uint8_t> data{0xE8, 0x03};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, false, data, result);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, OversizedFdPayloadIsUndecodable) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // A CAN FD frame (payload > 8 bytes) on a known id: the pinned decoder cannot
  // parse it, and the caller must see kUndecodable, not a silent empty result.
  const std::vector<std::uint8_t> data(12, 0);
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, false, data, result);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, UnknownIdIsNotMatched) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  DecodeResult result = DecodeResult::kDecoded;
  const auto sigs = dec.decode(999, false, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kNoMatch);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, OutOfRangeSignalLayoutIsDropped) {
  // A malformed DBC declaring a 65-bit signal (or a start bit past the 64-bit
  // payload) would make the pinned decoder shift by >= 64 — undefined behavior.
  // Such a message must be dropped at load, not registered and decoded.
  const char* const kBadDbc = R"DBC(VERSION "1.0.0"

NS_ :
	BA_
	BA_DEF_
	BA_DEF_DEF_
	BS_
	CM_
	VAL_
	VAL_TABLE_

BS_:

BU_: ECU

BO_ 100 Wide: 8 ECU
 SG_ TooWide : 0|65@1+ (1,0) [0|0] "" ECU

BO_ 101 Good: 8 ECU
 SG_ Ok : 0|16@1+ (1,0) [0|65535] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kBadDbc).has_value());
  // Only the well-formed message survives.
  EXPECT_EQ(dec.messageCount(), 1u);

  DecodeResult result = DecodeResult::kDecoded;
  const auto sigs = dec.decode(100, false, std::vector<std::uint8_t>(8, 0xFF), result);
  EXPECT_EQ(result, DecodeResult::kNoMatch);
  EXPECT_TRUE(sigs.empty());

  DecodeResult ok_result = DecodeResult::kNoMatch;
  EXPECT_FALSE(
      dec.decode(101, false, std::vector<std::uint8_t>(8, 0), ok_result).empty() &&
      ok_result != DecodeResult::kDecoded);
  EXPECT_EQ(ok_result, DecodeResult::kDecoded);
}

TEST(CanDecoder, GarbageDbcYieldsNoMessages) {
  CanDecoder dec;
  // Whether it throws (caught) or no-ops, no messages must be registered.
  (void)dec.loadDbcString("this is definitely not a dbc file");
  EXPECT_EQ(dec.messageCount(), 0u);
}

TEST(CanTopic, RendersHexIds) {
  EXPECT_EQ(mf4_detail::hexId(0x0u), "0x0");
  EXPECT_EQ(mf4_detail::hexId(0x100u), "0x100");
  EXPECT_EQ(mf4_detail::hexId(0x18FEF100u), "0x18FEF100");
  EXPECT_EQ(mf4_detail::hexId(0xFFFFFFFFu), "0xFFFFFFFF");
}

// Shared topic naming for both CAN loaders: frames from different buses must
// land in different topics ("CAN/ch<N>/..."), channel 0 (unknown bus) omits
// the channel segment, and unnamed messages fall back to the hex id.
TEST(CanTopic, NamesTopicsByChannelAndMessage) {
  EXPECT_EQ(mf4_detail::canTopicName(1, "EngineData", 0x100u), "CAN/ch1/EngineData");
  EXPECT_EQ(mf4_detail::canTopicName(2, "", 0x1ABu), "CAN/ch2/0x1AB");
  EXPECT_EQ(mf4_detail::canTopicName(0, "EngineData", 0x100u), "CAN/EngineData");
  EXPECT_EQ(mf4_detail::canTopicName(0, "", 0x7FFu), "CAN/0x7FF");
}

}  // namespace
