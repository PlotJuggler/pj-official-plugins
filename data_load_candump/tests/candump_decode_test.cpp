// End-to-end tests: fixture file -> candump_parser -> pj_can_dbc::CanDecoder,
// the same pipeline candump_source.cpp drives (which itself needs a live PJ
// host and so is not unit-tested directly -- mirrors
// data_load_blf/tests/blf_frames_test.cpp's BlfDecode.* tests).

#include <gtest/gtest.h>

#include <cstdint>
#include <field_test_helpers.hpp>
#include <fstream>
#include <iterator>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/signal_row.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../candump_parser.hpp"
#include "../csv_dictionary.hpp"
#include "test_support.hpp"

namespace {

using candump_detail::LineKind;
using candump_detail::ParsedLine;
using candump_detail::parseLine;
using candump_test::testDataPath;
using pj_can_dbc::CanDecoder;
using pj_can_dbc::DecodedSignal;
using pj_can_dbc::DecodeResult;
using pj_can_dbc::SignalRowBuilder;
using pj_can_dbc::testing::findField;

std::string readFile(const std::string& name) {
  std::ifstream file(testDataPath(name.c_str()));
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<std::string> readLines(const std::string& name) {
  std::istringstream stream(readFile(name));
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

const DecodedSignal* find(const std::vector<DecodedSignal>& sigs, const std::string& name) {
  for (const auto& s : sigs) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

/// Parses every line, tallying each recognized-but-unsupported kind (RTR,
/// error, FD, XL, `-e` detail, DROPCOUNT) and collecting the kData lines for
/// the caller's own decode assertions -- the "parse, switch on kind, tally
/// or fall through to kData" scaffolding both fixture-decode tests below
/// otherwise duplicate. kMalformed/kWallClockTs are never expected by either
/// fixture, so they fail the line immediately either way.
struct KindCounts {
  int rtr = 0;
  int error = 0;
  int fd = 0;
  int xl = 0;
  int error_detail = 0;
  int dropcount = 0;
  std::uint64_t dropped_total = 0;
  std::string dropcount_iface;
  std::vector<ParsedLine> data_lines;
};

KindCounts countKinds(const std::vector<std::string>& lines) {
  KindCounts counts;
  for (const auto& line : lines) {
    const auto parsed = parseLine(line);
    switch (parsed.kind) {
      case LineKind::kRtr:
        ++counts.rtr;
        break;
      case LineKind::kError:
        ++counts.error;
        break;
      case LineKind::kFd:
        ++counts.fd;
        break;
      case LineKind::kXl:
        ++counts.xl;
        break;
      case LineKind::kErrorDetail:
        ++counts.error_detail;
        break;
      case LineKind::kDropCount:
        ++counts.dropcount;
        counts.dropped_total += parsed.dropped_count;
        counts.dropcount_iface = parsed.interface;
        EXPECT_FALSE(parsed.has_timestamp) << "DROPCOUNT carries no timestamp upstream";
        break;
      case LineKind::kMalformed:
      case LineKind::kWallClockTs:
        ADD_FAILURE() << "unexpected kind for: " << line;
        break;
      case LineKind::kData:
        counts.data_lines.push_back(parsed);
        break;
    }
  }
  return counts;
}

// log_format.log + sample.dbc: classic + extended decodable frames, plus one
// of each recognized-but-unsupported kind, counted correctly. Fixture shapes
// verified against linux-can/can-utils lib.c snprintf_canframe (see
// candump_parser.hpp's grammar comment for exact line references).
TEST(CandumpDecode, LogFormatFixtureDecodesThroughDbc) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcFile(testDataPath("sample.dbc")).has_value());

  const auto lines = readLines("log_format.log");
  ASSERT_EQ(lines.size(), 11u);

  const KindCounts counts = countKinds(lines);
  EXPECT_EQ(counts.rtr, 1);
  EXPECT_EQ(counts.error, 1);
  EXPECT_EQ(counts.fd, 1);
  EXPECT_EQ(counts.xl, 1);
  EXPECT_EQ(counts.error_detail, 0);  // this fixture's `-e` lines are screen_format.txt's job
  EXPECT_EQ(counts.dropcount, 1);
  EXPECT_EQ(counts.dropped_total, 3u);
  EXPECT_EQ(counts.dropcount_iface, "can0");

  int decoded = 0, empty_payload_ok = 0;
  double speed_value = -1.0;
  double ext_sig_value = -1.0;
  double under_value = -1.0;

  for (const auto& parsed : counts.data_lines) {
    if (parsed.can_id == 0x500u) {
      EXPECT_TRUE(parsed.data.empty());
      ++empty_payload_ok;
      continue;
    }
    DecodeResult result = DecodeResult::kNoMatch;
    const auto sigs = dec.decode(parsed.can_id, parsed.extended, parsed.data, result);
    ASSERT_EQ(result, DecodeResult::kDecoded);
    ++decoded;
    if (const auto* speed = find(sigs, "Speed")) {
      speed_value = speed->value;
    }
    if (const auto* ext = find(sigs, "ExtSig")) {
      ext_sig_value = ext->value;
    }
    if (const auto* under = find(sigs, "Under")) {
      under_value = under->value;
    }
  }

  EXPECT_EQ(empty_payload_ok, 1);
  // Standard "100#E803" decodes 3 times (plain, with " R" suffix, and via the
  // dotted/dashed interface + ns timestamp); extended "000004D2" once; the
  // "_F" len8_dlc-suffixed classic frame ("600#...._F") once.
  EXPECT_EQ(decoded, 5);
  EXPECT_DOUBLE_EQ(speed_value, 100.0);
  EXPECT_DOUBLE_EQ(ext_sig_value, 1.0);
  EXPECT_DOUBLE_EQ(under_value, 0.0);
}

// sample.dbc's VAL_ tables through the SAME SignalRowBuilder path
// candump_source.cpp's decoded-frame branch uses: Speed's raw value (1000,
// pinned above) matches its table exactly; ExtSig's raw value (1) has no
// matching entry (the table only has key 5) and must fall back to its
// number as text.
TEST(CandumpDecode, ValueTableLabelsRouteThroughSignalRowBuilder) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcFile(testDataPath("sample.dbc")).has_value());

  DecodeResult result = DecodeResult::kNoMatch;
  const auto speed_sigs = dec.decode(0x100u, /*extended=*/false, std::vector<std::uint8_t>{0xE8, 0x03}, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  SignalRowBuilder speed_builder;
  const auto speed_fields = speed_builder.build(speed_sigs);
  const auto* speed_label = findField(speed_fields, "Speed_label");
  ASSERT_NE(speed_label, nullptr);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(speed_label->value));
  EXPECT_EQ(std::get<std::string_view>(speed_label->value), "HUNDRED_KMH");

  const std::vector<std::uint8_t> ext_data{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const auto ext_sigs = dec.decode(0x4D2u, /*extended=*/true, ext_data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  SignalRowBuilder ext_builder;
  const auto ext_fields = ext_builder.build(ext_sigs);
  const auto* ext_label = findField(ext_fields, "ExtSig_label");
  ASSERT_NE(ext_label, nullptr);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(ext_label->value));
  EXPECT_EQ(std::get<std::string_view>(ext_label->value), "1");
}

// screen_format.txt + sample.dbc. Fixture shapes verified against
// lib.c snprintf_long_canframe (see candump_parser.hpp's grammar comment).
TEST(CandumpDecode, ScreenFormatFixtureDecodesThroughDbc) {
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcFile(testDataPath("sample.dbc")).has_value());

  const auto lines = readLines("screen_format.txt");
  ASSERT_EQ(lines.size(), 11u);

  const KindCounts counts = countKinds(lines);
  EXPECT_EQ(counts.fd, 1);
  EXPECT_EQ(counts.error, 1);
  EXPECT_EQ(counts.error_detail, 1);
  EXPECT_EQ(counts.rtr, 1);
  EXPECT_EQ(counts.xl, 1);
  EXPECT_EQ(counts.dropcount, 0);  // this fixture's DROPCOUNT line is log_format.log's job

  for (const auto& parsed : counts.data_lines) {
    if (parsed.can_id == 0x200u) {
      EXPECT_TRUE(parsed.direction_known);
      EXPECT_TRUE(parsed.is_tx);
      EXPECT_TRUE(parsed.data.empty());
      continue;
    }
    DecodeResult result = DecodeResult::kNoMatch;
    const auto sigs = dec.decode(parsed.can_id, parsed.extended, parsed.data, result);
    ASSERT_EQ(result, DecodeResult::kDecoded);
    if (parsed.can_id == 0x600u) {
      // The "{F}" LEN8_DLC-brace fixture -- decodes through UnderscoreDlc,
      // not EngineData; raw byte0 = 0x11 = 17.
      const auto* under = find(sigs, "Under");
      ASSERT_NE(under, nullptr);
      EXPECT_DOUBLE_EQ(under->value, 17.0);
      continue;
    }
    const auto* speed = find(sigs, "Speed");
    ASSERT_NE(speed, nullptr);
    EXPECT_DOUBLE_EQ(speed->value, 100.0);
  }
  // kData lines: plain "100", RX "100", empty-payload TX "200", the
  // brace-DLC "600" (decodes through UnderscoreDlc), the SFF-indented "100"
  // repeat, and the short-interface "c0" "100" repeat.
  EXPECT_EQ(counts.data_lines.size(), 6u);
}

// arus_race.txt + arus_subset.csv -> DBC: full pipeline reproducing the
// worked example verified against the real ARUS CSV (log_plotter semantics).
TEST(CandumpDecode, ArusRaceFixtureMatchesWorkedExample) {
  std::string dbc_text, warnings;
  ASSERT_TRUE(candump_detail::arusCsvToDbc(readFile("arus_subset.csv"), dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value()) << dbc_text;

  const auto lines = readLines("arus_race.txt");
  ASSERT_EQ(lines.size(), 5u);

  struct Expected {
    std::string signal;
    double value;
  };
  const std::vector<std::vector<Expected>> expected = {
      {{"IMU_ax", -2.0}, {"IMU_ay", 2.0}},    {{"fl_inv_speed", 10.47197551196}, {"fl_inv_torque", 9.8}},
      {{"brake_hydr_front", -40.9239940387}}, {{"brake_hydr_front", -100.5365126677}},
      {{"extensometer", 0.445271946587}},
  };

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto parsed = parseLine(lines[i]);
    ASSERT_EQ(parsed.kind, LineKind::kData) << lines[i];
    DecodeResult result = DecodeResult::kNoMatch;
    const auto sigs = dec.decode(parsed.can_id, parsed.extended, parsed.data, result);
    ASSERT_EQ(result, DecodeResult::kDecoded) << lines[i];
    for (const auto& exp : expected[i]) {
      const auto* sig = find(sigs, exp.signal);
      ASSERT_NE(sig, nullptr) << exp.signal << " missing on line " << lines[i];
      EXPECT_NEAR(sig->value, exp.value, 1e-9) << exp.signal;
    }
  }
}

TEST(CandumpDecode, RelativeTzFixtureIsMonotonicAndSmall) {
  std::ifstream stream(testDataPath("relative_tz.log"));
  const auto detection = candump_detail::detectTimeMode(stream, 1000);
  EXPECT_TRUE(detection.saw_numeric_timestamp);
  EXPECT_EQ(detection.mode, candump_detail::TimeMode::kRelativeMonotonic);
}

TEST(CandumpDecode, RelativeTdFixtureIsNonMonotonic) {
  std::ifstream stream(testDataPath("relative_td.log"));
  const auto detection = candump_detail::detectTimeMode(stream, 1000);
  EXPECT_TRUE(detection.saw_numeric_timestamp);
  EXPECT_EQ(detection.mode, candump_detail::TimeMode::kRelativeDelta);
}

}  // namespace
