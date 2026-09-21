#include "../candump_parser.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>

namespace {

using candump_detail::detectTimeMode;
using candump_detail::LineKind;
using candump_detail::parseLine;
using candump_detail::parseLogLine;
using candump_detail::parseScreenLine;
using candump_detail::rawTimestampNs;
using candump_detail::TimeMode;

// --- Log format: classic data frames ---

TEST(CandumpParserLog, DecodesStandardFrameMicroseconds) {
  const auto line = parseLogLine("(1700000000.123456) can0 100#E803");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_TRUE(line.has_timestamp);
  EXPECT_EQ(line.ts_seconds, 1700000000);
  EXPECT_EQ(line.ts_fraction_ns, 123456000);
  EXPECT_EQ(line.interface, "can0");
  EXPECT_EQ(line.can_id, 0x100u);
  EXPECT_FALSE(line.extended);
  ASSERT_EQ(line.data.size(), 2u);
  EXPECT_EQ(line.data[0], 0xE8);
  EXPECT_EQ(line.data[1], 0x03);
  EXPECT_FALSE(line.direction_known);
}

TEST(CandumpParserLog, NanosecondFractionAndDottedDashedInterface) {
  const auto line = parseLogLine("(1700000000.183456789) can-eth0.1 100#E803");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_EQ(line.ts_fraction_ns, 183456789);
  EXPECT_EQ(line.interface, "can-eth0.1");
}

TEST(CandumpParserLog, DecodesExtendedFrame) {
  const auto line = parseLogLine("(1700000000.133456) can0 000004D2#0102030405060708");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_TRUE(line.extended);
  EXPECT_EQ(line.can_id, 0x4D2u);
  ASSERT_EQ(line.data.size(), 8u);
  EXPECT_EQ(line.data[0], 0x01);
  EXPECT_EQ(line.data[7], 0x08);
}

TEST(CandumpParserLog, EmptyPayloadIsValidData) {
  const auto line = parseLogLine("(1700000000.203456) can0 500#");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_TRUE(line.data.empty());
}

TEST(CandumpParserLog, RecognizesRtr) {
  const auto line = parseLogLine("(1700000000.143456) can0 200#R");
  EXPECT_EQ(line.kind, LineKind::kRtr);
  EXPECT_EQ(line.can_id, 0x200u);
}

TEST(CandumpParserLog, RecognizesRtrWithRequestedDlcDigits) {
  const auto line = parseLogLine("(0.0) can0 200#R8");
  EXPECT_EQ(line.kind, LineKind::kRtr);
}

TEST(CandumpParserLog, RecognizesErrorFrameByIdFlag) {
  const auto line = parseLogLine("(1700000000.153456) can0 20000020#0000000000000000");
  EXPECT_EQ(line.kind, LineKind::kError);
}

TEST(CandumpParserLog, RecognizesFdFrame) {
  const auto line = parseLogLine("(1700000000.163456) can0 300##2AABBCCDDEEFF0011");
  EXPECT_EQ(line.kind, LineKind::kFd);
  EXPECT_EQ(line.can_id, 0x300u);
}

TEST(CandumpParserLog, RecognizesXlFrame) {
  // Real shape: "%02X%03X#%02X:%02X:%08X#<data>" (vcid+prio#flags:sdt:af#data),
  // can-utils lib.c snprintf_canframe ~L343-346.
  const auto line = parseLogLine("(1700000000.173456) can0 00123#11:22:12345678#AABB");
  EXPECT_EQ(line.kind, LineKind::kXl);
}

TEST(CandumpParserLog, RecognizesUnderscoreLen8DlcSuffix) {
  // A classic 8-byte frame may carry an optional raw len8_dlc suffix
  // "_<hex-digit>" (lib.c ~L428-435, CC_DLC_DELIM='_').
  const auto line = parseLogLine("(1700000000.213456) can0 600#0011223344556677_F");
  ASSERT_EQ(line.kind, LineKind::kData);
  ASSERT_EQ(line.data.size(), 8u);
  EXPECT_EQ(line.data[0], 0x00);
  EXPECT_EQ(line.data[7], 0x77);
}

TEST(CandumpParserLog, MalformedUnderscoreSuffixIsMalformed) {
  EXPECT_EQ(parseLogLine("(0.0) can0 100#E803_ZZ").kind, LineKind::kMalformed);
  EXPECT_EQ(parseLogLine("(0.0) can0 100#E803_").kind, LineKind::kMalformed);
}

TEST(CandumpParserLog, TrailingDirectionSuffix) {
  const auto rx = parseLogLine("(1700000000.183456) can0 100#E803 R");
  ASSERT_EQ(rx.kind, LineKind::kData);
  EXPECT_TRUE(rx.direction_known);
  EXPECT_FALSE(rx.is_tx);

  const auto tx = parseLogLine("(1700000000.183456) can0 100#E803 T");
  ASSERT_EQ(tx.kind, LineKind::kData);
  EXPECT_TRUE(tx.direction_known);
  EXPECT_TRUE(tx.is_tx);
}

TEST(CandumpParserLog, RecognizesWallClockTimestampAsUnsupported) {
  const auto line = parseLogLine("(2024-01-15 10:23:45.123456) can0 100#E803");
  EXPECT_EQ(line.kind, LineKind::kWallClockTs);
  EXPECT_TRUE(line.has_timestamp);
}

TEST(CandumpParserLog, NoTimestampIsMalformed) {
  const auto line = parseLogLine("can0 100#E803");
  EXPECT_EQ(line.kind, LineKind::kMalformed);
  EXPECT_FALSE(line.has_timestamp);
}

TEST(CandumpParserLog, MissingHashIsMalformed) {
  EXPECT_EQ(parseLogLine("(0.0) can0 100").kind, LineKind::kMalformed);
}

TEST(CandumpParserLog, NonHexIdIsMalformed) {
  EXPECT_EQ(parseLogLine("(0.0) can0 10G#E803").kind, LineKind::kMalformed);
}

TEST(CandumpParserLog, OddHexDigitDataIsMalformed) {
  EXPECT_EQ(parseLogLine("(0.0) can0 100#E80").kind, LineKind::kMalformed);
}

TEST(CandumpParserLog, TooManyDataBytesIsMalformed) {
  EXPECT_EQ(parseLogLine("(0.0) can0 100#0011223344556677889900").kind, LineKind::kMalformed);
}

TEST(CandumpParserLog, EmptyLineIsMalformed) {
  EXPECT_EQ(parseLogLine("").kind, LineKind::kMalformed);
}

// --- Screen format ---
// Grammar verified against linux-can/can-utils lib.c snprintf_long_canframe
// (see candump_parser.hpp's top-of-file comment for exact line references).

TEST(CandumpParserScreen, LeadingSpaceBeforeTimestampIsRequired) {
  // Screen format always has a leading space (candump.c's
  // `sprintf(afrbuf, " %s", ...)`); log format never does. Both are accepted.
  const auto line = parseScreenLine(" (1700000000.123456)  can0  100   [2]  E8 03");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_EQ(line.interface, "can0");
}

TEST(CandumpParserScreen, DecodesPlainFrame) {
  const auto line = parseScreenLine(" (1700000000.123456)  can0  100   [2]  E8 03");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_EQ(line.interface, "can0");
  EXPECT_EQ(line.can_id, 0x100u);
  ASSERT_EQ(line.data.size(), 2u);
  EXPECT_EQ(line.data[0], 0xE8);
  EXPECT_EQ(line.data[1], 0x03);
}

TEST(CandumpParserScreen, DirectionColumnWithFlagPairBeforeFrame) {
  // `-x`: "  RX"/"  TX" then ALWAYS a flag pair (extra_fd_info, e.g. "- -"
  // for CC/FD) -- the flag string has an embedded space, so it tokenizes as
  // TWO symbols (candump.c ~L897-909).
  const auto rx = parseScreenLine(" (1700000000.133456)  can0  RX - -  100   [2]  E8 03");
  ASSERT_EQ(rx.kind, LineKind::kData);
  EXPECT_TRUE(rx.direction_known);
  EXPECT_FALSE(rx.is_tx);
  EXPECT_EQ(rx.can_id, 0x100u);

  const auto tx = parseScreenLine(" (1700000000.143456)  can0  TX - -  200   [0]");
  ASSERT_EQ(tx.kind, LineKind::kData);
  EXPECT_TRUE(tx.direction_known);
  EXPECT_TRUE(tx.is_tx);
  EXPECT_TRUE(tx.data.empty());
}

TEST(CandumpParserScreen, TwoDigitBracketIsFd) {
  // CAN FD length is always "[%02d]" (2 digits, zero-padded) regardless of
  // the actual byte count -- lib.c ~L583-588.
  const auto line = parseScreenLine(" (1700000000.153456)  can0  300  [12]  AA BB CC DD EE FF 00 11 22 33 44 55");
  EXPECT_EQ(line.kind, LineKind::kFd);
  EXPECT_EQ(line.can_id, 0x300u);
}

TEST(CandumpParserScreen, TwoDigitBracketIsFdEvenForShortPayload) {
  // len=4 still prints as "[04]" (2 digits) for an FD frame, not "[4]".
  const auto line = parseScreenLine(" (1700000000.153456)  can0  301  [04]  AA BB CC DD");
  EXPECT_EQ(line.kind, LineKind::kFd);
}

TEST(CandumpParserScreen, RecognizesErrorFrameByIdFlag) {
  const auto line = parseScreenLine(" (1700000000.163456)  can0  20000020   [8]  00 00 00 00 00 00 00 00   ERRORFRAME");
  EXPECT_EQ(line.kind, LineKind::kError);
}

TEST(CandumpParserScreen, RecognizesRemoteRequestAsRtr) {
  // "[N]  remote request" (DLC bracket, THEN the two words) -- lib.c
  // ~L579-582, not a bare "R"/"remote" token right after the id.
  const auto line = parseScreenLine(" (1700000000.173456)  can0  400   [2]  remote request");
  EXPECT_EQ(line.kind, LineKind::kRtr);
  EXPECT_EQ(line.can_id, 0x400u);
}

TEST(CandumpParserScreen, RecognizesLenEightDlcBrace) {
  // "{H}" (LEN8_DLC view): one HEX digit, not the byte count -- payload
  // length comes from bytes actually present (coordinator correction).
  const auto line = parseScreenLine(" (1700000000.193456)  can0  600   {F}  11 22 33 44 55 66 77 88");
  ASSERT_EQ(line.kind, LineKind::kData);
  ASSERT_EQ(line.data.size(), 8u);
  EXPECT_EQ(line.data[0], 0x11);
  EXPECT_EQ(line.data[7], 0x88);
}

TEST(CandumpParserScreen, RecognizesCanXlFrame) {
  // "123 [0004] (00|11:22:12345678) AA BB" -- 3-digit prio id, 4-digit
  // length, then a "(vcid|flags:sdt:af)" group -- lib.c ~L473-497.
  const auto line = parseScreenLine(" (1700000000.183456)  can0  123 [0004] (00|11:22:12345678) AA BB");
  EXPECT_EQ(line.kind, LineKind::kXl);
}

TEST(CandumpParserScreen, SffIndentAfterEffIsToleratedByWhitespaceSkipping) {
  // CANLIB_VIEW_INDENT_SFF adds 5 extra spaces before an SFF id once an EFF
  // frame has been seen in the same dump session -- pure whitespace, no
  // special-casing needed.
  const auto line = parseScreenLine(" (1700000000.203456)  can0       100   [2]  E8 03");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_EQ(line.can_id, 0x100u);
}

TEST(CandumpParserScreen, RightPaddedShortInterfaceName) {
  // Interfaces are right-aligned ("%*s") to the longest name in the dump
  // session, so a short name gets extra LEADING spaces.
  const auto line = parseScreenLine(" (1700000000.213456)    c0  100   [2]  E8 03");
  ASSERT_EQ(line.kind, LineKind::kData);
  EXPECT_EQ(line.interface, "c0");
}

TEST(CandumpParserScreen, MissingDlcBracketIsMalformed) {
  EXPECT_EQ(parseScreenLine("(0.0) can0 100 2 E8 03").kind, LineKind::kMalformed);
}

TEST(CandumpParserScreen, NonHexByteIsMalformed) {
  // "[1]" reliably means exactly 1 byte follows (lib.c writes `len + '0'`
  // directly), so a non-hex token there is a real error, unlike the
  // brace/LEN8_DLC case which just stops consuming.
  EXPECT_EQ(parseScreenLine("(0.0) can0 100 [1] ZZ").kind, LineKind::kMalformed);
}

// --- DROPCOUNT / error-detail: recognized only at the parseLine() dispatch
// level -- neither carries a timestamp/interface prefix in either grammar.

TEST(CandumpParserDispatch, RecognizesDropCountWithNoTimestampOrIfacePrefix) {
  const auto line = parseLine("DROPCOUNT: dropped 3 CAN frames on 'can0' socket (total drops 3)");
  EXPECT_EQ(line.kind, LineKind::kDropCount);
  EXPECT_FALSE(line.has_timestamp);
  EXPECT_EQ(line.interface, "can0");
  EXPECT_EQ(line.dropped_count, 3u);
}

TEST(CandumpParserDispatch, DropCountSingularFrame) {
  const auto line = parseLine("DROPCOUNT: dropped 1 CAN frame on 'vcan0' socket (total drops 7)");
  EXPECT_EQ(line.kind, LineKind::kDropCount);
  EXPECT_EQ(line.interface, "vcan0");
  EXPECT_EQ(line.dropped_count, 1u);
}

TEST(CandumpParserDispatch, DropCountUnparsableMessageStillRecognized) {
  const auto line = parseLine("DROPCOUNT: something unexpected");
  EXPECT_EQ(line.kind, LineKind::kDropCount);
  EXPECT_EQ(line.dropped_count, 0u);
  EXPECT_TRUE(line.interface.empty());
}

TEST(CandumpParserDispatch, RecognizesErrorDetailContinuationLine) {
  const auto line = parseLine("\tbus-off");
  EXPECT_EQ(line.kind, LineKind::kErrorDetail);
}

// --- Dispatcher ---

TEST(CandumpParserDispatch, TriesLogThenScreen) {
  EXPECT_EQ(parseLine("(0.0) can0 100#E803").kind, LineKind::kData);
  EXPECT_EQ(parseLine("(0.0) can0 100 [2] E8 03").kind, LineKind::kData);
  EXPECT_EQ(parseLine("this is not candump at all").kind, LineKind::kMalformed);
}

// --- Timestamp structure / raw ns ---

TEST(CandumpTimestamp, RawNsCombinesSecondsAndFraction) {
  const auto line = parseLogLine("(5.250000) can0 100#E803");
  EXPECT_EQ(rawTimestampNs(line), 5'250'000'000LL);
}

TEST(CandumpTimestamp, RawNsSaturatesOnOverflow) {
  candump_detail::ParsedLine line;
  line.ts_seconds = std::numeric_limits<std::int64_t>::max() / 1'000'000'000 + 10;
  line.ts_fraction_ns = 0;
  EXPECT_EQ(rawTimestampNs(line), std::numeric_limits<std::int64_t>::max());
}

// --- detectTimeMode ---

TEST(CandumpTimeMode, AbsoluteWhenFirstSecondsAboveThreshold) {
  std::istringstream in("(1700000000.000000) can0 100#E803\n(1700000000.010000) can0 100#E803\n");
  const auto det = detectTimeMode(in, 1000);
  EXPECT_TRUE(det.saw_numeric_timestamp);
  EXPECT_EQ(det.mode, TimeMode::kAbsolute);
}

TEST(CandumpTimeMode, RelativeMonotonicWhenIncreasingAndSmall) {
  std::istringstream in("(0.000000) can0 100#E803\n(0.010000) can0 100#E803\n(0.025000) can0 100#E803\n");
  const auto det = detectTimeMode(in, 1000);
  EXPECT_TRUE(det.saw_numeric_timestamp);
  EXPECT_EQ(det.mode, TimeMode::kRelativeMonotonic);
}

TEST(CandumpTimeMode, RelativeDeltaWhenNonMonotonicAndSmall) {
  std::istringstream in("(0.500000) can0 100#E803\n(0.100000) can0 100#E803\n(0.300000) can0 100#E803\n");
  const auto det = detectTimeMode(in, 1000);
  EXPECT_TRUE(det.saw_numeric_timestamp);
  EXPECT_EQ(det.mode, TimeMode::kRelativeDelta);
}

TEST(CandumpTimeMode, NoTimestampAtAllIsReported) {
  std::istringstream in("this is not candump\nneither is this\n");
  const auto det = detectTimeMode(in, 1000);
  EXPECT_FALSE(det.saw_numeric_timestamp);
}

TEST(CandumpTimeMode, WallClockOnlyLeavesNoNumericTimestampSeen) {
  std::istringstream in("(2024-01-15 10:23:45.123456) can0 100#E803\n");
  const auto det = detectTimeMode(in, 1000);
  EXPECT_FALSE(det.saw_numeric_timestamp);
}

TEST(CandumpTimeMode, StopsAtMaxLines) {
  // Only the first (non-monotonic-triggering) line is scanned when capped at 1.
  std::istringstream in("(0.500000) can0 100#E803\n(0.100000) can0 100#E803\n");
  const auto det = detectTimeMode(in, 1);
  EXPECT_TRUE(det.saw_numeric_timestamp);
  EXPECT_EQ(det.mode, TimeMode::kRelativeMonotonic);  // only one sample: trivially monotonic
}

}  // namespace
