#include "../can_decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using mf4_detail::CanDecoder;
using mf4_detail::DecodedSignal;

// A minimal but complete DBC: one 8-byte message (id 256) with two
// little-endian unsigned signals. Speed scales by 0.1 (km/h); Rpm by 1 (rpm).
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
  EXPECT_EQ(dec.messageCount(), 1u);

  // Speed raw 1000 (0x03E8) -> 100.0 km/h; Rpm raw 3000 (0x0BB8) -> 3000 rpm.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0xB8, 0x0B, 0, 0, 0, 0};
  bool matched = false;
  const auto sigs = dec.decode(256, data, matched);
  EXPECT_TRUE(matched);
  ASSERT_EQ(sigs.size(), 2u);

  const auto* speed = find(sigs, "Speed");
  const auto* rpm = find(sigs, "Rpm");
  ASSERT_NE(speed, nullptr);
  ASSERT_NE(rpm, nullptr);
  EXPECT_DOUBLE_EQ(speed->value, 100.0);
  EXPECT_EQ(speed->unit, "km/h");
  EXPECT_DOUBLE_EQ(rpm->value, 3000.0);
  EXPECT_EQ(rpm->unit, "rpm");
}

TEST(CanDecoder, UnknownIdIsNotMatched) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  bool matched = true;
  const auto sigs = dec.decode(999, std::vector<std::uint8_t>(8, 0), matched);
  EXPECT_FALSE(matched);
  EXPECT_TRUE(sigs.empty());
}

TEST(CanDecoder, ExtendedIdFlagIsIgnoredWhenMatching) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kDbc).has_value());
  // The DBC extended-frame flag (bit 31) must not prevent a match on id 256.
  const std::vector<std::uint8_t> data{0xE8, 0x03, 0, 0, 0, 0, 0, 0};
  bool matched = false;
  const auto sigs = dec.decode(256u | 0x8000'0000u, data, matched);
  EXPECT_TRUE(matched);
  const auto* speed = find(sigs, "Speed");
  ASSERT_NE(speed, nullptr);
  EXPECT_DOUBLE_EQ(speed->value, 100.0);
}

TEST(CanDecoder, GarbageDbcYieldsNoMessages) {
  CanDecoder dec;
  // Whether it throws (caught) or no-ops, no messages must be registered.
  (void)dec.loadDbcString("this is definitely not a dbc file");
  EXPECT_EQ(dec.messageCount(), 0u);
}

}  // namespace
