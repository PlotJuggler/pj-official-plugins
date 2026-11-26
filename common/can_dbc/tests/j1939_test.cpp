#include <gtest/gtest.h>

#include <cstdint>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <pj_can_dbc/fast_packet.hpp>
#include <pj_can_dbc/j1939.hpp>
#include <string>
#include <vector>

namespace {

using pj_can_dbc::CanDecoder;
using pj_can_dbc::CanProtocol;
using pj_can_dbc::DecodedSignal;
using pj_can_dbc::DecodeResult;
using pj_can_dbc::FastPacketAssembler;

// An NMEA 2000-style DBC: 29-bit ids with the Vector flag, each written with
// one sample priority and source address 0xFE, as exporters do.
//  - VesselHeading, PGN 127250 (PDU2), id 0x09F112FE: 8 bytes.
//  - ISORequest, PGN 59904 (PDU1, destination 0xFF), id 0x18EAFFFE.
//  - PositionLike, PGN 129029 (PDU2), id 0x0DF805FE: 20 bytes, a fast packet.
//  - StdMsg: a standard 11-bit message, which every protocol still matches by id.
const char* const kN2kDbc = R"DBC(VERSION "1.0.0"

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

BO_ 2314277630 VesselHeading: 8 ECU
 SG_ SID : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ Heading : 8|16@1+ (0.0001,0) [0|6.5535] "rad" ECU

BO_ 2565537790 ISORequest: 3 ECU
 SG_ RequestedPGN : 0|24@1+ (1,0) [0|16777215] "" ECU

BO_ 2381841918 PositionLike: 20 ECU
 SG_ SID : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ Days : 8|16@1+ (1,0) [0|65535] "d" ECU
 SG_ Seconds : 24|32@1+ (0.0001,0) [0|0] "s" ECU
 SG_ Count : 56|8@1+ (1,0) [0|255] "" ECU
 SG_ Tail : 136|16@1+ (1,0) [0|65535] "" ECU

BO_ 256 StdMsg: 8 ECU
 SG_ Value : 0|8@1+ (1,0) [0|255] "" ECU
)DBC";

const DecodedSignal* find(const std::vector<DecodedSignal>& sigs, const std::string& name) {
  for (const auto& sig : sigs) {
    if (sig.name == name) {
      return &sig;
    }
  }
  return nullptr;
}

TEST(J1939Id, SplitsTheIdentifier) {
  // Priority 3, PGN 129029 (0x1F805, PDU2), source 0x10.
  EXPECT_EQ(pj_can_dbc::pgnFromCanId(0x0DF80510u), 129029u);
  EXPECT_EQ(pj_can_dbc::sourceAddress(0x0DF80510u), 0x10u);
  EXPECT_FALSE(pj_can_dbc::isPdu1(0x0DF80510u));
  // PDU1: PGN 59904 (0xEA00) to destination 0x23 from 0x10 -- the destination is not part of the PGN.
  EXPECT_EQ(pj_can_dbc::pgnFromCanId(0x18EA2310u), 59904u);
  EXPECT_TRUE(pj_can_dbc::isPdu1(0x18EA2310u));
  EXPECT_EQ(pj_can_dbc::pduSpecific(0x18EA2310u), 0x23u);
  // The data page bit is part of the PGN.
  EXPECT_EQ(pj_can_dbc::pgnFromCanId(0x19F00010u), 0x1F000u);
}

TEST(CanDecoderPgn, RawProtocolNeedsTheExactId) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  EXPECT_EQ(dec.protocol(), CanProtocol::kRaw);
  DecodeResult result = DecodeResult::kDecoded;
  // VesselHeading from source 0x10, not the DBC's 0xFE: no match by whole id.
  dec.decode(0x09F11210u, true, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kNoMatch);
}

TEST(CanDecoderPgn, MatchesAnySourceAndPriority) {
  for (const auto protocol : {CanProtocol::kJ1939, CanProtocol::kNmea2000}) {
    CanDecoder dec;
    ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
    dec.setProtocol(protocol);
    // Priority 6 instead of 2, source 0x10 instead of 0xFE. Heading raw 0x3039 = 12345 -> 1.2345 rad.
    const std::vector<std::uint8_t> data{0x07, 0x39, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    DecodeResult result = DecodeResult::kNoMatch;
    const auto sigs = dec.decode(0x19F11210u, true, data, result);
    ASSERT_EQ(result, DecodeResult::kDecoded);
    EXPECT_EQ(dec.messageName(0x19F11210u, true), "VesselHeading");
    const auto* heading = find(sigs, "Heading");
    ASSERT_NE(heading, nullptr);
    EXPECT_DOUBLE_EQ(heading->value, 1.2345);
  }
}

TEST(CanDecoderPgn, Pdu1MatchesAnyDestination) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  dec.setProtocol(CanProtocol::kJ1939);
  // ISORequest to destination 0x23 (the DBC says 0xFF) from 0x10, requesting PGN 60928 (0x00EE00).
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x18EA2310u, true, {0x00, 0xEE, 0x00}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* pgn = find(sigs, "RequestedPGN");
  ASSERT_NE(pgn, nullptr);
  EXPECT_DOUBLE_EQ(pgn->value, 60928.0);
}

TEST(CanDecoderPgn, StandardFramesStillMatchById) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  dec.setProtocol(CanProtocol::kJ1939);
  DecodeResult result = DecodeResult::kNoMatch;
  dec.decode(256, false, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  // An unknown PGN is no match.
  dec.decode(0x0DF00010u, true, std::vector<std::uint8_t>(8, 0), result);
  EXPECT_EQ(result, DecodeResult::kNoMatch);
}

TEST(CanDecoderPgn, FastPacketOnlyForLongMessagesUnderNmea2000) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  EXPECT_FALSE(dec.isFastPacket(0x0DF80510u, true));  // kRaw
  dec.setProtocol(CanProtocol::kJ1939);
  EXPECT_FALSE(dec.isFastPacket(0x0DF80510u, true));  // J1939 has no fast packets
  dec.setProtocol(CanProtocol::kNmea2000);
  EXPECT_TRUE(dec.isFastPacket(0x0DF80510u, true));    // PositionLike: 20 bytes
  EXPECT_FALSE(dec.isFastPacket(0x09F11210u, true));   // VesselHeading: 8 bytes
  EXPECT_FALSE(dec.isFastPacket(0x0DF00010u, true));   // unknown PGN
  EXPECT_FALSE(dec.isFastPacket(0x0DF80510u, false));  // standard frame
}

TEST(CanDecoderPgn, VariableLengthPayloadDecodesTheSignalsThatFit) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  dec.setProtocol(CanProtocol::kNmea2000);
  // 10 of PositionLike's 20 bytes: SID, Days, Seconds and Count fit; Tail (bytes 17..18) does not.
  const std::vector<std::uint8_t> data{0x01, 0x10, 0x27, 0x40, 0x42, 0x0F, 0x00, 0x05, 0xAA, 0xBB};
  DecodeResult result = DecodeResult::kNoMatch;
  auto sigs = dec.decode(0x0DF80510u, true, data, result, /*variable_length=*/true);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  ASSERT_EQ(sigs.size(), 4u);
  EXPECT_DOUBLE_EQ(find(sigs, "Days")->value, 10000.0);
  EXPECT_DOUBLE_EQ(find(sigs, "Seconds")->value, 100.0);  // 1000000 * 0.0001
  EXPECT_DOUBLE_EQ(find(sigs, "Count")->value, 5.0);
  EXPECT_EQ(find(sigs, "Tail"), nullptr);

  // Without variable_length the same payload is too short for the message.
  sigs = dec.decode(0x0DF80510u, true, data, result);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
  EXPECT_TRUE(sigs.empty());

  // Not even the first signal fits: undecodable either way.
  sigs = dec.decode(0x0DF80510u, true, {}, result, /*variable_length=*/true);
  EXPECT_EQ(result, DecodeResult::kUndecodable);
}

TEST(CanDecoderPgn, VariableLengthPayloadLongerThanTheMessage) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kN2kDbc).has_value());
  dec.setProtocol(CanProtocol::kNmea2000);
  // Repeated fields can make a payload longer than the DBC message: all its signals decode.
  std::vector<std::uint8_t> data(40, 0);
  data[17] = 0x34;
  data[18] = 0x12;
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x0DF80510u, true, data, result, /*variable_length=*/true);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  EXPECT_EQ(sigs.size(), 5u);
  EXPECT_DOUBLE_EQ(find(sigs, "Tail")->value, 4660.0);
}

// An OEM-style J1939 DBC.
//  - VECTOR__INDEPENDENT_SIG_MSG: the placeholder a Vector export keeps orphan
//    signals in. Its id, 0xC0000000, is no CAN id; with the extended flag
//    stripped it would read as PGN 0.
//  - TSC1, id 0x0C000003: the real PGN 0 message.
//  - PropB_Engine / PropB_Body, ids 0x18FF1011 / 0x18FF1022: one proprietary
//    PGN (0xFF10) written once per sender, with a different layout each.
//  - NoSignals: a 12-byte standard message that defines no signal.
const char* const kOemDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ENG BODY

BO_ 3221225472 VECTOR__INDEPENDENT_SIG_MSG: 0 Vector__XXX
 SG_ Orphan : 0|8@1+ (1,0) [0|255] "" Vector__XXX

BO_ 2348810243 TSC1: 8 ENG
 SG_ RequestedSpeed : 8|16@1+ (0.125,0) [0|8031.875] "rpm" ENG

BO_ 2566852625 PropB_Engine: 8 ENG
 SG_ OilPressure : 0|16@1+ (1,0) [0|65535] "kPa" BODY

BO_ 2566852642 PropB_Body: 8 BODY
 SG_ DoorState : 0|8@1+ (1,0) [0|255] "" ENG
 SG_ CabinTemp : 8|8@1+ (1,-40) [-40|215] "degC" ENG

BO_ 200 NoSignals: 12 ENG
)DBC";

TEST(CanDecoderPgn, PlaceholderMessageIsNotAPgn) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kOemDbc).has_value());
  dec.setProtocol(CanProtocol::kJ1939);
  // TSC1 from source 0x0B at priority 4: PGN 0, which the placeholder (loaded first) must not claim.
  EXPECT_EQ(dec.messageName(0x1000000Bu, true), "TSC1");
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x1000000Bu, true, {0x01, 0x40, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* speed = find(sigs, "RequestedSpeed");
  ASSERT_NE(speed, nullptr);
  EXPECT_DOUBLE_EQ(speed->value, 1000.0);  // 0x1F40 * 0.125
}

TEST(CanDecoderPgn, MessageWrittenForTheFramesSourceWins) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kOemDbc).has_value());
  dec.setProtocol(CanProtocol::kJ1939);
  EXPECT_EQ(dec.messageName(0x18FF1011u, true), "PropB_Engine");
  EXPECT_EQ(dec.messageName(0x18FF1022u, true), "PropB_Body");
  EXPECT_EQ(dec.messageName(0x0CFF1022u, true), "PropB_Body");    // priority is not part of the match
  EXPECT_EQ(dec.messageName(0x18FF1033u, true), "PropB_Engine");  // a source no message names: the PGN's first
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x18FF1022u, true, {0x02, 0x3C, 0, 0, 0, 0, 0, 0}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* door = find(sigs, "DoorState");
  const auto* temp = find(sigs, "CabinTemp");
  ASSERT_NE(door, nullptr);
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(door->value, 2.0);
  EXPECT_DOUBLE_EQ(temp->value, 20.0);  // 0x3C - 40
}

TEST(CanDecoderPgn, LongFrameOnAMessageWithoutSignalsDecodesToNothing) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(kOemDbc).has_value());
  // Nothing to read is not a failed read: only a variable-length payload needs one signal to fit.
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(200, false, std::vector<std::uint8_t>(12, 0), result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  EXPECT_TRUE(sigs.empty());
}

TEST(PgnTopicName, AddsSourceAndPdu1Destination) {
  EXPECT_EQ(pj_can_dbc::pgnTopicName("can0", "VesselHeading", 0x09F11210u), "CAN/can0/VesselHeading/0x10");
  EXPECT_EQ(pj_can_dbc::pgnTopicName("can0", "ISORequest", 0x18EA2305u), "CAN/can0/ISORequest/0x05/0x23");
  // A priority change keeps the topic.
  EXPECT_EQ(
      pj_can_dbc::pgnTopicName("can0", "VesselHeading", 0x09F11210u),
      pj_can_dbc::pgnTopicName("can0", "VesselHeading", 0x19F11210u));
}

// --- FastPacketAssembler ---

/// The frames of one fast-packet message: frame 0 = {seq|0, len, 6 bytes}, then {seq|i, 7 bytes}, padded with 0xFF.
std::vector<std::vector<std::uint8_t>> fastPacketFrames(
    std::uint8_t sequence, const std::vector<std::uint8_t>& payload) {
  std::vector<std::vector<std::uint8_t>> frames;
  std::size_t pos = 0;
  for (std::uint8_t index = 0; pos < payload.size() || index == 0; ++index) {
    std::vector<std::uint8_t> frame{static_cast<std::uint8_t>((sequence << 5) | index)};
    if (index == 0) {
      frame.push_back(static_cast<std::uint8_t>(payload.size()));
    }
    while (frame.size() < 8) {
      frame.push_back(pos < payload.size() ? payload[pos++] : 0xFF);
    }
    frames.push_back(frame);
  }
  return frames;
}

std::vector<std::uint8_t> counting(std::size_t length) {
  std::vector<std::uint8_t> payload(length);
  for (std::size_t i = 0; i < length; ++i) {
    payload[i] = static_cast<std::uint8_t>(i + 1);
  }
  return payload;
}

TEST(FastPacket, ReassemblesInOrderFrames) {
  FastPacketAssembler assembler;
  const auto payload = counting(20);  // 6 + 7 + 7: three frames
  const auto frames = fastPacketFrames(2, payload);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_FALSE(assembler.add(0x0DF80510u, frames[0], 100).has_value());
  EXPECT_FALSE(assembler.add(0x0DF80510u, frames[1], 200).has_value());
  const auto done = assembler.add(0x0DF80510u, frames[2], 300);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->payload, payload);  // trailing 0xFF padding cut at the announced length
  EXPECT_EQ(done->timestamp_ns, 100);
  EXPECT_EQ(assembler.droppedFrames(), 0u);
}

TEST(FastPacket, ShortMessageCompletesInFrameZero) {
  FastPacketAssembler assembler;
  const auto done = assembler.add(1, fastPacketFrames(0, counting(5))[0], 7);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->payload, counting(5));
}

TEST(FastPacket, LongestMessage) {
  FastPacketAssembler assembler;
  const auto payload = counting(FastPacketAssembler::kMaxPayloadBytes);
  const auto frames = fastPacketFrames(7, payload);
  ASSERT_EQ(frames.size(), 32u);
  std::optional<FastPacketAssembler::Message> done;
  for (const auto& frame : frames) {
    done = assembler.add(1, frame, 0);
  }
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->payload, payload);
}

TEST(FastPacket, InterleavedSourcesAssembleSeparately) {
  FastPacketAssembler assembler;
  const auto a = fastPacketFrames(1, counting(13));
  const auto b = fastPacketFrames(5, std::vector<std::uint8_t>(13, 0xAB));
  EXPECT_FALSE(assembler.add(0x0DF80504u, a[0], 1).has_value());
  EXPECT_FALSE(assembler.add(0x0DF80510u, b[0], 2).has_value());
  const auto done_b = assembler.add(0x0DF80510u, b[1], 3);
  const auto done_a = assembler.add(0x0DF80504u, a[1], 4);
  ASSERT_TRUE(done_a.has_value());
  ASSERT_TRUE(done_b.has_value());
  EXPECT_EQ(done_a->payload, counting(13));
  EXPECT_EQ(done_b->payload, std::vector<std::uint8_t>(13, 0xAB));
}

TEST(FastPacket, SkippedFrameDropsTheMessage) {
  FastPacketAssembler assembler;
  const auto frames = fastPacketFrames(3, counting(20));
  EXPECT_FALSE(assembler.add(1, frames[0], 0).has_value());
  EXPECT_FALSE(assembler.add(1, frames[2], 0).has_value());  // frame 1 lost
  EXPECT_EQ(assembler.droppedFrames(), 2u);                  // frame 0 and the out-of-order frame 2
  // The message stays dropped: the frame 1 that arrives late continues nothing.
  EXPECT_FALSE(assembler.add(1, frames[1], 0).has_value());
  EXPECT_EQ(assembler.droppedFrames(), 3u);
}

TEST(FastPacket, SequenceChangeDropsTheMessage) {
  FastPacketAssembler assembler;
  const auto first = fastPacketFrames(1, counting(13));
  const auto second = fastPacketFrames(2, counting(13));
  EXPECT_FALSE(assembler.add(1, first[0], 0).has_value());
  EXPECT_FALSE(assembler.add(1, second[1], 0).has_value());  // frame 1 of another message
  EXPECT_EQ(assembler.droppedFrames(), 2u);
}

TEST(FastPacket, NewFrameZeroRestartsTheMessage) {
  FastPacketAssembler assembler;
  const auto first = fastPacketFrames(1, counting(20));
  const auto second = fastPacketFrames(2, counting(13));
  EXPECT_FALSE(assembler.add(1, first[0], 0).has_value());
  EXPECT_FALSE(assembler.add(1, first[1], 0).has_value());
  EXPECT_FALSE(assembler.add(1, second[0], 50).has_value());  // the first message never finished
  const auto done = assembler.add(1, second[1], 60);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->payload, counting(13));
  EXPECT_EQ(done->timestamp_ns, 50);
  EXPECT_EQ(assembler.droppedFrames(), 2u);
}

TEST(FastPacket, MalformedFramesAreDropped) {
  FastPacketAssembler assembler;
  EXPECT_FALSE(assembler.add(1, {0x00}, 0).has_value());                         // too short
  EXPECT_FALSE(assembler.add(1, {0x00, 0x00, 1, 2, 3}, 0).has_value());          // zero length
  EXPECT_FALSE(assembler.add(1, {0x00, 224, 1, 2, 3, 4, 5, 6}, 0).has_value());  // over 223 bytes
  EXPECT_EQ(assembler.droppedFrames(), 3u);
}

TEST(FastPacket, ShortFrameBeforeTheLastDropsTheMessage) {
  FastPacketAssembler assembler;
  const auto frames = fastPacketFrames(1, counting(20));
  EXPECT_FALSE(assembler.add(1, frames[0], 0).has_value());
  // Frame 1 owes 7 bytes and brings 3: the bytes after it would land at the wrong offsets.
  const std::vector<std::uint8_t> cut(frames[1].begin(), frames[1].begin() + 4);
  EXPECT_FALSE(assembler.add(1, cut, 0).has_value());
  EXPECT_EQ(assembler.droppedFrames(), 2u);
  EXPECT_FALSE(assembler.add(1, frames[2], 0).has_value());  // continues nothing
  EXPECT_EQ(assembler.droppedFrames(), 3u);
}

TEST(FastPacket, ShortFrameZeroIsDropped) {
  FastPacketAssembler assembler;
  const auto frames = fastPacketFrames(1, counting(20));
  EXPECT_FALSE(assembler.add(1, {frames[0][0], 20, 1, 2}, 0).has_value());  // announces 20 bytes, brings 2 of its 6
  EXPECT_EQ(assembler.droppedFrames(), 1u);
  EXPECT_FALSE(assembler.add(1, frames[1], 0).has_value());  // continues nothing
  EXPECT_EQ(assembler.droppedFrames(), 2u);
}

TEST(FastPacket, UnfinishedMessageIsPending) {
  FastPacketAssembler assembler;
  const auto frames = fastPacketFrames(1, counting(20));
  EXPECT_FALSE(assembler.add(1, frames[0], 0).has_value());
  EXPECT_FALSE(assembler.add(1, frames[1], 0).has_value());
  // Not dropped yet, but lost if the input ends here: the importer adds these to its dropped count.
  EXPECT_EQ(assembler.droppedFrames(), 0u);
  EXPECT_EQ(assembler.pendingFrames(), 2u);
  EXPECT_TRUE(assembler.add(1, frames[2], 0).has_value());
  EXPECT_EQ(assembler.pendingFrames(), 0u);
}

TEST(FastPacket, LastFrameMayBeShort) {
  FastPacketAssembler assembler;
  const auto frames = fastPacketFrames(1, counting(8));
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_FALSE(assembler.add(1, frames[0], 0).has_value());
  // The last frame owes 2 bytes; a sender need not pad it to 8.
  const std::vector<std::uint8_t> last(frames[1].begin(), frames[1].begin() + 3);
  const auto done = assembler.add(1, last, 0);
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->payload, counting(8));
}

}  // namespace
