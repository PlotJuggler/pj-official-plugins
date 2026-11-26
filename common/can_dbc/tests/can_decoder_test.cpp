#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <field_test_helpers.hpp>
#include <iterator>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <pj_can_dbc/signal_row.hpp>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using pj_can_dbc::CanDecoder;
using pj_can_dbc::DecodedSignal;
using pj_can_dbc::DecodeResult;
using pj_can_dbc::SignalRowBuilder;
using pj_can_dbc::testing::findField;

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
  // Raw 0x12345678 (LE) -> 305419896. Upstream masks unsigned values with a 32-bit `1 << size`,
  // UB at size >= 32, which decodes every frame as 0; the patch in cmake/dbc_parser.cmake fixes it.
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

TEST(CanDecoder, LongerFrameOnClassicMessageDecodes) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // A 12-byte frame on the 8-byte EngineData: its signals lie in the first 8 bytes and decode as from a classic
  // frame; the extra bytes are ignored.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0xB8, 0x0B, 0xFF, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, false, data, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  const auto* speed = find(sigs, "Speed");
  const auto* rpm = find(sigs, "Rpm");
  const auto* temp = find(sigs, "Temp");
  ASSERT_NE(speed, nullptr);
  ASSERT_NE(rpm, nullptr);
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(speed->value, 100.0);
  EXPECT_DOUBLE_EQ(rpm->value, 3000.0);
  EXPECT_DOUBLE_EQ(temp->value, -1.0);
}

// Messages longer than 8 bytes (CAN FD, or an NMEA 2000 fast-packet payload). dbc_parser_cpp refuses payloads
// over 8 bytes; CanDecoder reads them itself with the same bit numbering.
//  - LongMsg (32 bytes): signals entirely past byte 7 -- Intel, Motorola, signed, a 64-bit one starting
//    mid-byte, and the last byte.
//  - ShortLayout (8 bytes) / LongLayout (16 bytes): the same signals in the first 8 bytes, to compare the two
//    decode paths bit for bit.
//  - PastEnd (16 bytes): a signal declared past the message's last byte.
//  - TooFar (8 bytes): a start bit past 64 in a classic message, still dropped.
const char* const kLongDbc = R"DBC(VERSION "1.0.0"

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

BO_ 768 LongMsg: 32 ECU
 SG_ LeHigh : 64|16@1+ (1,0) [0|65535] "" ECU
 SG_ BeHigh : 87|16@0+ (1,0) [0|65535] "" ECU
 SG_ SignedHigh : 96|12@1- (0.5,0) [-1024|1023.5] "" ECU
 SG_ Wide : 124|64@1+ (1,0) [0|0] "" ECU
 SG_ Tail : 248|8@1+ (1,0) [0|255] "" ECU

BO_ 769 ShortLayout: 8 ECU
 SG_ A : 3|12@1+ (1,0) [0|4095] "" ECU
 SG_ B : 13|12@0+ (1,0) [0|4095] "" ECU
 SG_ C : 16|8@1- (1,0) [-128|127] "" ECU
 SG_ D : 39|10@0- (0.25,-3) [0|0] "" ECU
 SG_ E : 40|16@1- (1,0) [-32768|32767] "" ECU
 SG_ F : 63|1@1+ (1,0) [0|1] "" ECU
 SG_ G : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ H : 52|11@0- (2,1) [0|0] "" ECU

BO_ 770 LongLayout: 16 ECU
 SG_ A : 3|12@1+ (1,0) [0|4095] "" ECU
 SG_ B : 13|12@0+ (1,0) [0|4095] "" ECU
 SG_ C : 16|8@1- (1,0) [-128|127] "" ECU
 SG_ D : 39|10@0- (0.25,-3) [0|0] "" ECU
 SG_ E : 40|16@1- (1,0) [-32768|32767] "" ECU
 SG_ F : 63|1@1+ (1,0) [0|1] "" ECU
 SG_ G : 7|8@0+ (1,0) [0|255] "" ECU
 SG_ H : 52|11@0- (2,1) [0|0] "" ECU

BO_ 771 PastEnd: 16 ECU
 SG_ Over : 120|16@1+ (1,0) [0|65535] "" ECU

BO_ 772 TooFar: 8 ECU
 SG_ Far : 64|8@1+ (1,0) [0|255] "" ECU
)DBC";

TEST(CanDecoderLongPayload, LoadsMessagesLongerThanEightBytes) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kLongDbc).has_value());
  // Every message but TooFar: a start bit past 64 is still out of range in an 8-byte message.
  EXPECT_EQ(dec.messageCount(), 4u);
  DecodeResult result = DecodeResult::kDecoded;
  EXPECT_TRUE(dec.decode(772, false, std::vector<std::uint8_t>(16, 0), result).empty());
  EXPECT_EQ(result, DecodeResult::kNoMatch);
}

TEST(CanDecoderLongPayload, DecodesSignalsPastByteSeven) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kLongDbc).has_value());
  std::vector<std::uint8_t> data(32, 0);
  data[8] = 0x34;  // LeHigh = 0x1234, little-endian in bytes 8..9
  data[9] = 0x12;
  data[10] = 0xAB;  // BeHigh = 0xABCD, big-endian in bytes 10..11
  data[11] = 0xCD;
  data[12] = 0xFE;  // SignedHigh = 12-bit 0xFFE = -2 -> * 0.5 = -1.0 (byte 12 + low nibble of byte 13)
  data[13] = 0x0F;
  // Wide = 0x0123456789ABCDEF from bit 124: its low nibble is byte 15's high nibble, the rest
  // (0x00123456789ABCDE) runs little-endian through bytes 16..23.
  data[15] = 0xF0;
  const std::uint8_t wide_rest[] = {0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x00};
  std::copy(std::begin(wide_rest), std::end(wide_rest), data.begin() + 16);
  data[31] = 0x5A;

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(768, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto value = [&sigs](const std::string& name) {
    const auto* sig = find(sigs, name);
    EXPECT_NE(sig, nullptr) << name;
    return sig != nullptr ? sig->value : 0.0;
  };
  EXPECT_DOUBLE_EQ(value("LeHigh"), 4660.0);
  EXPECT_DOUBLE_EQ(value("BeHigh"), 43981.0);
  EXPECT_DOUBLE_EQ(value("SignedHigh"), -1.0);
  EXPECT_DOUBLE_EQ(value("Wide"), static_cast<double>(0x0123456789ABCDEFull));
  EXPECT_DOUBLE_EQ(value("Tail"), 90.0);
}

TEST(CanDecoderLongPayload, ReadsBitsLikeTheClassicPath) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kLongDbc).has_value());
  // Bytes with high bits set, so the signed signals come out negative.
  const std::vector<std::uint8_t> classic{0xF3, 0x9A, 0x81, 0x7E, 0xC5, 0x2D, 0xB7, 0x48};
  std::vector<std::uint8_t> extended = classic;
  extended.resize(16, 0xFF);

  DecodeResult classic_result = DecodeResult::kNoMatch;
  DecodeResult long_result = DecodeResult::kNoMatch;
  const auto from_parser = dec.decode(769, false, classic, classic_result);
  const auto from_long_path = dec.decode(770, false, extended, long_result);
  ASSERT_EQ(classic_result, DecodeResult::kDecoded);
  ASSERT_EQ(long_result, DecodeResult::kDecoded);
  ASSERT_EQ(from_parser.size(), 8u);
  ASSERT_EQ(from_long_path.size(), from_parser.size());
  for (std::size_t i = 0; i < from_parser.size(); ++i) {
    EXPECT_EQ(from_long_path[i].name, from_parser[i].name);
    EXPECT_DOUBLE_EQ(from_long_path[i].value, from_parser[i].value) << from_parser[i].name;
  }
  // Pin a few values by hand too, so the comparison cannot pass on two equal mistakes.
  EXPECT_DOUBLE_EQ(find(from_long_path, "A")->value, 862.0);     // (0x9AF3 >> 3) & 0xFFF = 0x35E
  EXPECT_DOUBLE_EQ(find(from_long_path, "C")->value, -127.0);    // byte 2 = 0x81
  EXPECT_DOUBLE_EQ(find(from_long_path, "E")->value, -18643.0);  // bytes 5..6 LE = 0xB72D, signed
}

TEST(CanDecoderLongPayload, SignalPastThePayloadIsUndecodable) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kLongDbc).has_value());
  // PastEnd's signal runs from bit 120 to 135 of a 16-byte (128-bit) message.
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(771, false, std::vector<std::uint8_t>(16, 0), result);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoderLongPayload, FrameShorterThanLongMessageIsUndecodable) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kLongDbc).has_value());
  // LongMsg is 32 bytes; an 8-byte frame on its id (e.g. one fast-packet chunk) must not decode.
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(768, false, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
  EXPECT_TRUE(sigs.empty());
}

// The sizes where the two decode paths could disagree: 64-bit signed (Intel and Motorola), 63- and 32-bit signed,
// a 1-bit signed signal, a 64-bit unsigned Motorola one. ShortWide (8 bytes) goes through dbc_parser_cpp,
// LongWide (16 bytes, same layout) through CanDecoder's own reader.
//  - PastEndBe (16 bytes): a Motorola signal whose last bits lie past the message.
const char* const kWideDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 780 ShortWide: 8 ECU
 SG_ S64 : 0|64@1- (1,0) [0|0] "" ECU
 SG_ M64 : 7|64@0- (1,0) [0|0] "" ECU
 SG_ S63 : 0|63@1- (1,0) [0|0] "" ECU
 SG_ S32 : 16|32@1- (1,0) [0|0] "" ECU
 SG_ M32 : 23|32@0- (1,0) [0|0] "" ECU
 SG_ B1 : 5|1@1- (1,0) [0|1] "" ECU
 SG_ U64 : 7|64@0+ (1,0) [0|0] "" ECU

BO_ 781 LongWide: 16 ECU
 SG_ S64 : 0|64@1- (1,0) [0|0] "" ECU
 SG_ M64 : 7|64@0- (1,0) [0|0] "" ECU
 SG_ S63 : 0|63@1- (1,0) [0|0] "" ECU
 SG_ S32 : 16|32@1- (1,0) [0|0] "" ECU
 SG_ M32 : 23|32@0- (1,0) [0|0] "" ECU
 SG_ B1 : 5|1@1- (1,0) [0|1] "" ECU
 SG_ U64 : 7|64@0+ (1,0) [0|0] "" ECU

BO_ 782 PastEndBe: 16 ECU
 SG_ OverBe : 127|16@0+ (1,0) [0|65535] "" ECU
)DBC";

TEST(CanDecoderLongPayload, SignedAndWideSignalsReadLikeTheClassicPath) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kWideDbc).has_value());
  // Bits 63 and 62 (byte 7) and the top bit of bytes 0 and 2 are set: S64, M64, S63 and M32 are negative.
  const std::vector<std::uint8_t> classic{0xF3, 0x9A, 0x81, 0x7E, 0xC5, 0x2D, 0xB7, 0xC8};
  std::vector<std::uint8_t> extended = classic;
  extended.resize(16, 0xFF);

  DecodeResult classic_result = DecodeResult::kNoMatch;
  DecodeResult long_result = DecodeResult::kNoMatch;
  const auto from_parser = dec.decode(780, false, classic, classic_result);
  const auto from_long_path = dec.decode(781, false, extended, long_result);
  ASSERT_EQ(classic_result, DecodeResult::kDecoded);
  ASSERT_EQ(long_result, DecodeResult::kDecoded);
  ASSERT_EQ(from_parser.size(), 7u);
  ASSERT_EQ(from_long_path.size(), from_parser.size());
  for (std::size_t i = 0; i < from_parser.size(); ++i) {
    EXPECT_EQ(from_long_path[i].name, from_parser[i].name);
    EXPECT_DOUBLE_EQ(from_long_path[i].value, from_parser[i].value) << from_parser[i].name;
  }
  // Pinned by hand, so the comparison cannot pass on two equal mistakes.
  const auto value = [&from_parser](const std::string& name) {
    const auto* sig = find(from_parser, name);
    EXPECT_NE(sig, nullptr) << name;
    return sig != nullptr ? sig->value : 0.0;
  };
  const auto as_signed = [](std::uint64_t bits) { return static_cast<double>(static_cast<std::int64_t>(bits)); };
  EXPECT_DOUBLE_EQ(value("S64"), as_signed(0xC8B72DC57E819AF3ull));  // bytes 7..0
  EXPECT_DOUBLE_EQ(value("M64"), as_signed(0xF39A817EC52DB7C8ull));  // bytes 0..7
  EXPECT_DOUBLE_EQ(value("S63"), as_signed(0xC8B72DC57E819AF3ull));  // 0x48B7... with its sign bit (62) extended
  EXPECT_DOUBLE_EQ(value("S32"), static_cast<double>(0x2DC57E81));   // bytes 5..2, positive
  EXPECT_DOUBLE_EQ(value("M32"), as_signed(0xFFFFFFFF817EC52Dull));  // bytes 2..5, negative
  EXPECT_DOUBLE_EQ(value("B1"), 1.0);                                // bit 5 of 0xF3; a 1-bit signed signal stays 0/1
  EXPECT_DOUBLE_EQ(value("U64"), static_cast<double>(0xF39A817EC52DB7C8ull));
}

TEST(CanDecoderLongPayload, MotorolaSignalPastThePayloadIsUndecodable) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kWideDbc).has_value());
  // OverBe's MSB is the top bit of byte 15; its 16 bits run 8 bits past the 16-byte message.
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(782, false, std::vector<std::uint8_t>(16, 0), result);
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

// The dbc.cpp patch (cmake/dbc_parser.cmake): factor/offset/min/max share one signed,
// optional-exponent number pattern, and BO_/SG_ token separators tolerate tabs and repeated
// spaces. Upstream drops such a signal SILENTLY (the message still loads, with fewer signals), so
// every test below also pins messageCount()/signal presence, not just the decoded value.

TEST(CanDecoderDbcFix, DecodesNegativeFactorAndOffset) {
  // Upstream scalePattern is "Non negative float": a negative factor cannot match SG_ at all, so
  // upstream drops the whole signal.
  const char* const kFixDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 300 NegMsg: 8 ECU
 SG_ Neg : 0|8@1- (-2,-1) [0|0] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kFixDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);

  // raw -3 (0xFD, signed 8-bit) -> -3 * -2 + -1 = 5.
  const std::vector<std::uint8_t> data{0xFD, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(300, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* neg = find(sigs, "Neg");
  ASSERT_NE(neg, nullptr);
  EXPECT_DOUBLE_EQ(neg->value, 5.0);
}

TEST(CanDecoderDbcFix, DecodesExponentialFactorOffsetMinMax) {
  // Upstream floatPattern has no exponent support, and Vector DBCs routinely emit float signals
  // as "[-3.4E+38|3.4E+38]": upstream drops every one of them.
  const char* const kFixDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 301 ExpMsg: 8 ECU
 SG_ Exp : 0|16@1+ (1E-2,1E+1) [-3.4E+38|3.4E+38] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kFixDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);

  // raw 500 (0x01F4) -> 500 * 0.01 + 10 = 15.
  const std::vector<std::uint8_t> data{0xF4, 0x01, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(301, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* exp_sig = find(sigs, "Exp");
  ASSERT_NE(exp_sig, nullptr);
  EXPECT_DOUBLE_EQ(exp_sig->value, 15.0);
}

TEST(CanDecoderDbcFix, DecodesLeadingPlusSignAndLeadingDotNumbers) {
  // "+2" (explicit plus) and ".5" (no leading digit) are both valid DBC
  // numbers that neither upstream pattern accepted.
  const char* const kFixDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 302 PlusDotMsg: 8 ECU
 SG_ PlusDot : 0|8@1+ (+2,.5) [0|0] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kFixDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);

  // raw 3 -> 3 * 2 + 0.5 = 6.5.
  const std::vector<std::uint8_t> data{3, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(302, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* pd = find(sigs, "PlusDot");
  ASSERT_NE(pd, nullptr);
  EXPECT_DOUBLE_EQ(pd->value, 6.5);
}

TEST(CanDecoderDbcFix, TabsAndDoubleSpacesDoNotDropTheSignal) {
  // Upstream whiteSpace is exactly one "\\s": a tab or a doubled space between SG_ tokens (both
  // legal DBC whitespace) fails signal_re and the signal is silently dropped. message_re's own
  // whitespace is patched separately and covered below.
  const char* const kFixDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 303 SpacedMsg: 8 ECU
	SG_	Spaced  :	0|8@1+	(2,0)  [0|0]  "" 	ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kFixDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);

  const std::vector<std::uint8_t> data{5, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(303, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* spaced = find(sigs, "Spaced");
  ASSERT_NE(spaced, nullptr);
  EXPECT_DOUBLE_EQ(spaced->value, 10.0);
}

TEST(CanDecoderDbcFix, MessageLineToleratesTabsAndDoubleSpaces) {
  // Same robustness check, but on the BO_ line's own whitespace (message_re).
  const char* const kFixDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_	304  SpacedMsg2:	8  ECU
 SG_ S : 0|8@1+ (1,0) [0|0] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kFixDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);

  const std::vector<std::uint8_t> data{7, 0, 0, 0, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(304, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  EXPECT_NE(find(sigs, "S"), nullptr);
}

// --- VAL_ value tables: a DBC "VAL_" line maps a signal's raw integer value
// to a text label (an enum-like status). CanDecoder resolves the label at
// decode time (DecodedSignal::raw/label) and SignalRowBuilder turns that
// into a "<signal>_label" text field alongside the signal's own unchanged
// numeric field, for PJ4's State Transitions view.

// Assembles "VERSION ... NS_ ... BS_: ... BU_: ECU" boilerplate plus one
// BO_/SG_ pair and (if non-empty) a trailing VAL_ line -- shared by the VAL_
// tests below instead of each repeating the same DBC header text.
std::string valDbc(const std::string& bo_line, const std::string& sg_line, const std::string& val_line) {
  std::string dbc = "VERSION \"1.0.0\"\n\nNS_ :\n\nBS_:\n\nBU_: ECU\n\n" + bo_line + "\n " + sg_line + "\n";
  if (!val_line.empty()) {
    dbc += val_line + "\n";
  }
  return dbc;
}

// BO_ 600 StatusMsg / AS_status [0|2], reused verbatim by two tests below.
const std::string kStatusValDbc = valDbc(
    "BO_ 600 StatusMsg: 8 ECU", R"(SG_ AS_status : 0|8@1+ (1,0) [0|2] "" ECU)",
    R"(VAL_ 600 AS_status 0 "OFF" 1 "READY" 2 "DRIVING" ;)");

TEST(CanDecoderValueTable, BasicTableResolvesLabel) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kStatusValDbc).has_value());
  EXPECT_EQ(dec.valueTableCount(), 1u);

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(600, false, std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* status = find(sigs, "AS_status");
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(status->raw.has_value());
  ASSERT_TRUE(status->label.has_value());
  EXPECT_EQ(*status->label, "READY");
  EXPECT_EQ(*status->raw, 1);
}

TEST(CanDecoderValueTable, NegativeInt8KeyResolvesLabel) {
  // Signed 8-bit signal, VAL_ key written as a literal negative number.
  const std::string dbc = valDbc(
      "BO_ 601 ErrMsgA: 8 ECU", R"(SG_ ErrSig : 0|8@1- (1,0) [-128|127] "" ECU)", R"(VAL_ 601 ErrSig -1 "ERROR" ;)");
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(601, false, std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* err = find(sigs, "ErrSig");
  ASSERT_NE(err, nullptr);
  EXPECT_DOUBLE_EQ(err->value, -1.0);
  ASSERT_TRUE(err->label.has_value());
  EXPECT_EQ(*err->label, "ERROR");
  EXPECT_EQ(*err->raw, -1);
}

TEST(CanDecoderValueTable, AllUnsignedInt8KeyConventionResolvesSameLabel) {
  // Same signal/payload as above, but the VAL_ key is spelled the OTHER
  // convention some DBC exporters use for a signed signal's negative value:
  // the all-unsigned 8-bit bit pattern (255, not -1). Both must resolve the
  // SAME raw key (-1) to the SAME label.
  const std::string dbc = valDbc(
      "BO_ 602 ErrMsgB: 8 ECU", R"(SG_ ErrSig : 0|8@1- (1,0) [-128|127] "" ECU)", R"(VAL_ 602 ErrSig 255 "ERROR" ;)");
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(602, false, std::vector<std::uint8_t>{0xFF, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* err = find(sigs, "ErrSig");
  ASSERT_NE(err, nullptr);
  ASSERT_TRUE(err->label.has_value());
  EXPECT_EQ(*err->label, "ERROR");
  EXPECT_EQ(*err->raw, -1);
}

TEST(CanDecoderValueTable, OutOfTableValueFallsBackToNumber) {
  const std::string dbc = valDbc(
      "BO_ 600 StatusMsg: 8 ECU", R"(SG_ AS_status : 0|8@1+ (1,0) [0|7] "" ECU)",
      R"(VAL_ 600 AS_status 0 "OFF" 1 "READY" 2 "DRIVING" ;)");
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(600, false, std::vector<std::uint8_t>{7, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* status = find(sigs, "AS_status");
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(status->raw.has_value());
  EXPECT_FALSE(status->label.has_value());
  EXPECT_EQ(*status->raw, 7);

  SignalRowBuilder builder;
  const auto fields = builder.build(sigs);
  const auto* label_field = findField(fields, "AS_status_label");
  ASSERT_NE(label_field, nullptr);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(label_field->value));
  EXPECT_EQ(std::get<std::string_view>(label_field->value), "7");
}

TEST(CanDecoderValueTable, ScaledSignalResolvesLabelFromRawKey) {
  // factor 0.5, offset 10: raw 4 -> physical 12.0; the VAL_ key (4) is the
  // RAW value, not the physical one.
  const std::string dbc = valDbc(
      "BO_ 603 ScaledMsg: 8 ECU", R"(SG_ Scaled : 0|8@1+ (0.5,10) [0|137.5] "" ECU)", R"(VAL_ 603 Scaled 4 "FOUR" ;)");
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(603, false, std::vector<std::uint8_t>{4, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* scaled = find(sigs, "Scaled");
  ASSERT_NE(scaled, nullptr);
  EXPECT_DOUBLE_EQ(scaled->value, 12.0);
  ASSERT_TRUE(scaled->label.has_value());
  EXPECT_EQ(*scaled->label, "FOUR");
  EXPECT_EQ(*scaled->raw, 4);
}

namespace {
// One DBC body per whitespace/punctuation variant of the SAME two-entry
// VAL_ line -- all must parse to the SAME two labels ("A" for key 0, "B" for
// key 1), pinning the vendored value_re/description_re whitespace fix.
void expectTwoEntryTableResolves(const std::string& val_line) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(valDbc("BO_ 604 VMsg: 8 ECU", R"(SG_ VField : 0|8@1+ (1,0) [0|1] "" ECU)", val_line))
                  .has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  auto sigs = dec.decode(604, false, std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* v0 = find(sigs, "VField");
  ASSERT_NE(v0, nullptr);
  ASSERT_TRUE(v0->label.has_value());
  EXPECT_EQ(*v0->label, "A");

  sigs = dec.decode(604, false, std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0}, result);
  const auto* v1 = find(sigs, "VField");
  ASSERT_NE(v1, nullptr);
  ASSERT_TRUE(v1->label.has_value());
  EXPECT_EQ(*v1->label, "B");
}
}  // namespace

TEST(CanDecoderValueTable, TabSeparatedEntriesParse) {
  expectTwoEntryTableResolves("VAL_\t604\tVField\t0\t\"A\"\t1\t\"B\"\t;");
}

TEST(CanDecoderValueTable, DoubleSpaceSeparatedEntriesParse) {
  expectTwoEntryTableResolves("VAL_  604  VField  0  \"A\"  1  \"B\"  ;");
}

TEST(CanDecoderValueTable, NoSpaceBeforeSemicolonParses) {
  expectTwoEntryTableResolves("VAL_ 604 VField 0 \"A\" 1 \"B\";");
}

TEST(CanDecoderValueTable, TrailingSpaceAfterSemicolonParses) {
  expectTwoEntryTableResolves("VAL_ 604 VField 0 \"A\" 1 \"B\" ; ");
}

TEST(CanDecoderValueTable, AppliesToExtendedMessageId) {
  // BO_ 2147484253 = 0x80000000 (Vector extended flag) | 605.
  const std::string dbc = valDbc(
      "BO_ 2147484253 ExtStatusMsg: 8 ECU", R"(SG_ ExtStatus : 0|8@1+ (1,0) [0|255] "" ECU)",
      R"(VAL_ 2147484253 ExtStatus 1 "ON" ;)");
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(605u, /*extended=*/true, std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* status = find(sigs, "ExtStatus");
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(status->label.has_value());
  EXPECT_EQ(*status->label, "ON");
}

TEST(CanDecoderValueTable, LoneValTableLineLoadsWithoutEffect) {
  // VAL_TABLE_ (a global enum table, not tied to any signal) is out of
  // scope for this fix -- it must not match value_re (its own line starts
  // with "VAL_" too) and must not throw or otherwise disturb the rest of
  // the DBC. Structurally different from valDbc()'s shape (VAL_TABLE_ sits
  // BEFORE the message, and there is no signal VAL_ line at all), so this
  // DBC is its own literal rather than a valDbc() call.
  const char* const kValDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

VAL_TABLE_ T 0 "A" ;

BO_ 606 PlainMsg: 8 ECU
 SG_ Plain : 0|8@1+ (1,0) [0|255] "" ECU
)DBC";
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kValDbc).has_value());
  EXPECT_EQ(dec.messageCount(), 1u);
  EXPECT_EQ(dec.valueTableCount(), 0u);

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(606, false, std::vector<std::uint8_t>{9, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* plain = find(sigs, "Plain");
  ASSERT_NE(plain, nullptr);
  EXPECT_FALSE(plain->raw.has_value());
}

TEST(CanDecoderValueTable, SignalWithoutValLineHasNoLabelField) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0xB8, 0x0B, 0, 0, 0, 0};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(256, false, data, result);
  const auto* speed = find(sigs, "Speed");
  ASSERT_NE(speed, nullptr);
  EXPECT_FALSE(speed->raw.has_value());

  SignalRowBuilder builder;
  const auto fields = builder.build(sigs);
  EXPECT_NE(findField(fields, "Speed"), nullptr);
  EXPECT_EQ(findField(fields, "Speed_label"), nullptr);
}

TEST(CanDecoderValueTable, WideTableParsesWithoutCrashing) {
  std::ostringstream val;
  val << "VAL_ 607 Wide";
  for (int i = 0; i < 256; ++i) {
    val << " " << i << " \"L" << i << "\"";
  }
  val << " ;";

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(
                     valDbc("BO_ 607 WideMsg: 8 ECU", R"(SG_ Wide : 0|32@1+ (1,0) [0|4294967295] "" ECU)", val.str()))
                  .has_value());
  EXPECT_EQ(dec.valueTableCount(), 1u);

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(607, false, std::vector<std::uint8_t>{42, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* wide = find(sigs, "Wide");
  ASSERT_NE(wide, nullptr);
  ASSERT_TRUE(wide->label.has_value());
  EXPECT_EQ(*wide->label, "L42");
}

TEST(CanDecoderValueTable, SemicolonInsideLabelKeepsEveryEntry) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(valDbc(
                                    "BO_ 608 SemiMsg: 8 ECU", R"(SG_ Semi : 0|8@1+ (1,0) [0|1] "" ECU)",
                                    R"(VAL_ 608 Semi 0 "a;b" 1 "c" ;)"))
                  .has_value());
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(608, false, std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* semi = find(sigs, "Semi");
  ASSERT_NE(semi, nullptr);
  ASSERT_TRUE(semi->label.has_value());
  EXPECT_EQ(*semi->label, "a;b");
}

TEST(CanDecoderValueTable, BuilderFieldOrderAndTypes) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kStatusValDbc).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(600, false, std::vector<std::uint8_t>{2, 0, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);

  SignalRowBuilder builder;
  const auto fields = builder.build(sigs);
  ASSERT_EQ(fields.size(), 2u);
  EXPECT_EQ(fields[0].name, "AS_status");
  ASSERT_TRUE(std::holds_alternative<double>(fields[0].value));
  EXPECT_DOUBLE_EQ(std::get<double>(fields[0].value), 2.0);
  EXPECT_EQ(fields[1].name, "AS_status_label");
  ASSERT_TRUE(std::holds_alternative<std::string_view>(fields[1].value));
  EXPECT_EQ(std::get<std::string_view>(fields[1].value), "DRIVING");
}

// --- A value-table label must never share a name with a real signal (see
// assignLabelNames in can_decoder.cpp).

namespace {

// Decodes one frame of message 700 and builds its row; owns the decoder and
// builder because the returned fields view their storage.
struct LabelRow {
  explicit LabelRow(const std::string& dbc, std::vector<std::uint8_t> payload) {
    loaded = dec.loadDbcString(dbc).has_value();
    DecodeResult result = DecodeResult::kNoMatch;
    sigs = dec.decode(700, false, payload, result);
    decoded = result == DecodeResult::kDecoded;
    fields = builder.build(sigs);
  }

  bool namesUnique() const {
    std::set<std::string> names;
    for (const auto& field : fields) {
      names.insert(field.name);
    }
    return names.size() == fields.size();
  }

  void expectText(const char* name, const char* text) const {
    const auto* field = findField(fields, name);
    ASSERT_NE(field, nullptr) << name;
    ASSERT_TRUE(std::holds_alternative<std::string_view>(field->value)) << name;
    EXPECT_EQ(std::get<std::string_view>(field->value), text) << name;
  }

  void expectNumber(const char* name, double value) const {
    const auto* field = findField(fields, name);
    ASSERT_NE(field, nullptr) << name;
    ASSERT_TRUE(std::holds_alternative<double>(field->value)) << name;
    EXPECT_DOUBLE_EQ(std::get<double>(field->value), value) << name;
  }

  CanDecoder dec;
  SignalRowBuilder builder;
  std::vector<pj_can_dbc::DecodedSignal> sigs;
  PJ::Span<const PJ::sdk::NamedFieldValue> fields;
  bool loaded = false;
  bool decoded = false;
};

constexpr const char* kFooSignal = R"(SG_ Foo : 0|8@1+ (1,0) [0|1] "" ECU)";
constexpr const char* kFooLabelSignal = R"(SG_ Foo_label : 8|8@1+ (1,0) [0|255] "" ECU)";
constexpr const char* kFooValTable = R"(VAL_ 700 Foo 0 "OFF" 1 "ON" ;)";

}  // namespace

TEST(CanDecoderValueTable, RealLabelSignalGetsSuffixedTextFieldInEitherOrder) {
  for (const bool label_first : {false, true}) {
    SCOPED_TRACE(label_first ? "Foo_label declared first" : "Foo declared first");
    const std::string signals = label_first ? std::string(kFooLabelSignal) + "\n " + kFooSignal
                                            : std::string(kFooSignal) + "\n " + kFooLabelSignal;
    const LabelRow row(valDbc("BO_ 700 LabelMsg: 8 ECU", signals, kFooValTable), {1, 7, 0, 0, 0, 0, 0, 0});
    ASSERT_TRUE(row.loaded && row.decoded);
    ASSERT_EQ(row.fields.size(), 3u);
    EXPECT_TRUE(row.namesUnique());
    row.expectNumber("Foo", 1.0);
    row.expectText("Foo_label_2", "ON");
    row.expectNumber("Foo_label", 7.0);
  }
}

TEST(CanDecoderValueTable, ChainedLabelSignals) {
  const std::string signals = std::string(kFooSignal) + "\n " + R"(SG_ Foo_label : 8|8@1+ (1,0) [0|1] "" ECU)";
  const std::string vals = std::string(kFooValTable) + "\n" + R"(VAL_ 700 Foo_label 0 "LOW" 1 "HIGH" ;)";
  const LabelRow row(valDbc("BO_ 700 LabelMsg: 8 ECU", signals, vals), {1, 0, 0, 0, 0, 0, 0, 0});
  ASSERT_TRUE(row.loaded && row.decoded);
  ASSERT_EQ(row.fields.size(), 4u);
  EXPECT_TRUE(row.namesUnique());
  row.expectNumber("Foo", 1.0);
  row.expectText("Foo_label_2", "ON");
  row.expectNumber("Foo_label", 0.0);
  row.expectText("Foo_label_label", "LOW");
}

TEST(CanDecoderValueTable, SuffixSkipsTakenNames) {
  const std::string signals =
      std::string(kFooSignal) + "\n " + kFooLabelSignal + "\n " + R"(SG_ Foo_label_2 : 16|8@1+ (1,0) [0|255] "" ECU)";
  const LabelRow row(valDbc("BO_ 700 LabelMsg: 8 ECU", signals, kFooValTable), {1, 7, 9, 0, 0, 0, 0, 0});
  ASSERT_TRUE(row.loaded && row.decoded);
  ASSERT_EQ(row.fields.size(), 4u);
  EXPECT_TRUE(row.namesUnique());
  row.expectText("Foo_label_3", "ON");
  row.expectNumber("Foo_label", 7.0);
  row.expectNumber("Foo_label_2", 9.0);
}

TEST(CanDecoderValueTable, LabelInAnotherMessageDoesNotForceSuffix) {
  const std::string dbc = valDbc(
      "BO_ 700 LabelMsg: 8 ECU", kFooSignal,
      std::string("BO_ 701 OtherMsg: 8 ECU\n ") + R"(SG_ Foo_label : 0|8@1+ (1,0) [0|255] "" ECU)" + "\n\n" +
          kFooValTable);
  const LabelRow row(dbc, {1, 0, 0, 0, 0, 0, 0, 0});
  ASSERT_TRUE(row.loaded && row.decoded);
  ASSERT_EQ(row.fields.size(), 2u);
  row.expectNumber("Foo", 1.0);
  row.expectText("Foo_label", "ON");
}

TEST(CanTopic, RendersHexIds) {
  EXPECT_EQ(pj_can_dbc::hexId(0x0u), "0x0");
  EXPECT_EQ(pj_can_dbc::hexId(0x100u), "0x100");
  EXPECT_EQ(pj_can_dbc::hexId(0x18FEF100u), "0x18FEF100");
  EXPECT_EQ(pj_can_dbc::hexId(0xFFFFFFFFu), "0xFFFFFFFF");
}

// Shared topic naming for both CAN loaders: frames from different buses must
// land in different topics ("CAN/ch<N>/..."), channel 0 (unknown bus) omits
// the channel segment, and unnamed messages fall back to the hex id.
TEST(CanTopic, NamesTopicsByChannelAndMessage) {
  EXPECT_EQ(pj_can_dbc::canTopicName(1, "EngineData", 0x100u), "CAN/ch1/EngineData");
  EXPECT_EQ(pj_can_dbc::canTopicName(2, "", 0x1ABu), "CAN/ch2/0x1AB");
  EXPECT_EQ(pj_can_dbc::canTopicName(0, "EngineData", 0x100u), "CAN/EngineData");
  EXPECT_EQ(pj_can_dbc::canTopicName(0, "", 0x7FFu), "CAN/0x7FF");
}

// data_load_candump's bus is a name (candump interface, e.g. "can0"), not a number.
TEST(CanTopic, NamesTopicsByInterfaceNameAndMessage) {
  EXPECT_EQ(pj_can_dbc::canTopicName(std::string_view("can0"), "EngineData", 0x100u), "CAN/can0/EngineData");
  EXPECT_EQ(pj_can_dbc::canTopicName(std::string_view("vcan0.1"), "", 0x1ABu), "CAN/vcan0.1/0x1AB");
  EXPECT_EQ(pj_can_dbc::canTopicName(std::string_view(""), "EngineData", 0x100u), "CAN/EngineData");
  EXPECT_EQ(pj_can_dbc::canTopicName(std::string_view(""), "", 0x7FFu), "CAN/0x7FF");
}

// The parser throws "Missing the required version header" when its ifstream fails to open; a
// missing file must report "not found" instead.
TEST(CanDecoder, LoadDbcFileReportsMissingFileAsNotFound) {
  CanDecoder decoder;
  const auto status = decoder.loadDbcFile("/nonexistent_pj_can_dbc_dir/missing.dbc");
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("DBC file not found: "), std::string::npos) << status.error();
  EXPECT_EQ(status.error().find("version header"), std::string::npos) << status.error();
}

// Libdbc only accepts a case-sensitive ".dbc" suffix; the gate runs before the file is touched.
TEST(CanDecoder, LoadDbcFileRejectsAWrongExtension) {
  CanDecoder decoder;
  const auto status = decoder.loadDbcFile("/nonexistent_pj_can_dbc_dir/vehicle.DBC");
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("Expected a .dbc extension"), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("DBC parse error"), std::string::npos) << status.error();
}

}  // namespace
