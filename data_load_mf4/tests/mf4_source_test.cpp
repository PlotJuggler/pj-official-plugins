#include <gtest/gtest.h>
#include <mdf/mdfreader.h>

#include <cstdint>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/signal_row.hpp>
#include <string>
#include <string_view>
#include <vector>

// Phase 1 scaffold check: mdflib links into the test executable and its reader
// reports a bad path cleanly. Real reader/value/can_decoder unit tests (with
// synthetic mdflib-writer fixtures) land in Phases 2-4.
TEST(Mf4Scaffold, MdflibLinksAndReportsBadFile) {
  // Explicit std::string: a bare literal is ambiguous between MdfReader's
  // std::string and std::string_view overloads.
  mdf::MdfReader reader(std::string("/nonexistent/path/does-not-exist.mf4"));
  EXPECT_FALSE(reader.IsOk());
}

// Helper-level test for the CAN-decode path importData() runs for MF4 "CAN
// bus logging" groups: a synthetic frame decoded through a DBC with a VAL_
// value table, fed to the SAME SignalRowBuilder importData() uses (see
// mf4_source.cpp's readCanGroup callback -- it calls can_row_builder.build()
// directly on the decoded signals, so this pins that exact routing without
// needing a real CAN-bus-logging MF4 fixture).
TEST(Mf4CanDecode, DecodedSignalsRouteThroughSignalRowBuilder) {
  const char* const kDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 700 GearMsg: 8 ECU
 SG_ Gear : 0|8@1+ (1,0) [0|255] "" ECU
VAL_ 700 Gear 2 "SECOND" ;
)DBC";

  pj_can_dbc::CanDecoder decoder;
  ASSERT_TRUE(decoder.loadDbcString(kDbc).has_value());

  const std::vector<std::uint8_t> data{2, 0, 0, 0, 0, 0, 0, 0};
  pj_can_dbc::DecodeResult result = pj_can_dbc::DecodeResult::kNoMatch;
  const auto signals = decoder.decode(700, /*extended=*/false, data, result);
  ASSERT_EQ(result, pj_can_dbc::DecodeResult::kDecoded);

  pj_can_dbc::SignalRowBuilder builder;
  const auto fields = builder.build(signals);
  ASSERT_EQ(fields.size(), 2u);
  EXPECT_EQ(fields[0].name, "Gear");
  EXPECT_EQ(fields[1].name, "Gear_label");
  ASSERT_TRUE(std::holds_alternative<std::string_view>(fields[1].value));
  EXPECT_EQ(std::get<std::string_view>(fields[1].value), "SECOND");
}
