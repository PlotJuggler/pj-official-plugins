#include <gtest/gtest.h>

#include <cstdint>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>
#include <vector>

namespace {

using mf4_detail::CanDecoder;
using mf4_detail::DecodedSignal;

// A minimal but complete DBC with:
//  - a standard 8-byte message id 256 (0x100): Speed (LE unsigned, x0.1 km/h),
//    Rpm (LE unsigned), Temp (LE signed 8-bit, degC);
//  - a Vector-flagged extended message 0x80000100 (= id 0x100, extended): ExtSig.
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
  EXPECT_EQ(dec.messageCount(), 2u);

  // Speed raw 1000 (0x03E8) -> 100.0 km/h; Rpm raw 3000 (0x0BB8) -> 3000 rpm.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0xB8, 0x0B, 0, 0, 0, 0};
  bool matched = false;
  const auto sigs = dec.decode(256, /*extended=*/false, data, matched);
  EXPECT_TRUE(matched);

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
  bool matched = false;
  const auto sigs = dec.decode(256, false, data, matched);
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
  bool matched = false;
  const auto sigs = dec.decode(0x100u, /*extended=*/true, data, matched);
  EXPECT_TRUE(matched);
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
  bool matched = false;
  const auto sigs = dec.decode(0x100u, false, data, matched);
  EXPECT_TRUE(matched);
  EXPECT_NE(find(sigs, "Speed"), nullptr);
  EXPECT_EQ(find(sigs, "ExtSig"), nullptr);
}

TEST(CanDecoder, TruncatedFrameIsRejected) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // EngineData is 8 bytes; a 2-byte frame must not decode to garbage.
  const std::vector<std::uint8_t> data{0xE8, 0x03};
  bool matched = false;
  const auto sigs = dec.decode(256, false, data, matched);
  EXPECT_TRUE(matched);       // the id is known
  EXPECT_TRUE(sigs.empty());  // but the frame is too short to decode
}

TEST(CanDecoder, UnknownIdIsNotMatched) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  bool matched = true;
  const auto sigs = dec.decode(999, false, std::vector<std::uint8_t>(8, 0), matched);
  EXPECT_FALSE(matched);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, GarbageDbcYieldsNoMessages) {
  CanDecoder dec;
  // Whether it throws (caught) or no-ops, no messages must be registered.
  (void)dec.loadDbcString("this is definitely not a dbc file");
  EXPECT_EQ(dec.messageCount(), 0u);
}

}  // namespace
