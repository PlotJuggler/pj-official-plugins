// J1939 / NMEA 2000 interfaces: the protocol saved per interface, and imports of test_data/n2k.log (see
// test_data/README.md) through the BUILT plugin.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "../candump_dialog.hpp"
#include "source_import_harness.hpp"
#include "test_support.hpp"

namespace {

using candump_detail::CandumpDialog;
using candump_test::testDataPath;
using nlohmann::json;
using pj_can_dbc::CanProtocol;
using pj_can_dbc::testing::ImportCapture;
using pj_can_dbc::testing::runImport;

ImportCapture importN2kLog(const json& protocols) {
  json cfg{{"filepath", testDataPath("n2k.log")}, {"iface_dicts", {{"vcan0", {testDataPath("n2k.dbc")}}}}};
  if (!protocols.is_null()) {
    cfg["iface_protocols"] = protocols;
  }
  return runImport(CANDUMP_SOURCE_PLUGIN_PATH, cfg.dump());
}

TEST(CandumpProtocolConfig, RoundTripsAndSkipsBadEntries) {
  CandumpDialog dialog;
  const std::string config =
      json{
          {"filepath", ""},
          {"iface_protocols",
           {{"can0", "j1939"}, {"can1", "nmea2000"}, {"can2", "raw"}, {"can3", "canopen"}, {"bad/name", "j1939"}}}}
          .dump();
  ASSERT_TRUE(dialog.loadConfig(config));
  EXPECT_EQ(dialog.interfaceProtocols().size(), 2u);
  EXPECT_EQ(dialog.interfaceProtocols().at("can0"), CanProtocol::kJ1939);
  EXPECT_EQ(dialog.interfaceProtocols().at("can1"), CanProtocol::kNmea2000);

  const auto saved = json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["iface_protocols"], (json{{"can0", "j1939"}, {"can1", "nmea2000"}}));
}

TEST(CandumpProtocolConfig, AllRawSavesNoKey) {
  // Raw is the default: nothing is saved for it.
  CandumpDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(json{{"filepath", ""}}.dump()));
  EXPECT_FALSE(json::parse(dialog.saveConfig()).contains("iface_protocols"));
}

TEST(CandumpProtocolDialog, ComboSetsTheSelectedInterface) {
  CandumpDialog dialog;
  dialog.setFilePath(testDataPath("n2k.log"));
  ASSERT_TRUE(dialog.onSelectionChanged("tableInterfaces", {"vcan0"}));

  auto shown = json::parse(dialog.widget_data());
  EXPECT_EQ(shown["comboProtocol"]["items"], (json{"Raw CAN", "J1939", "NMEA 2000"}));
  EXPECT_EQ(shown["comboProtocol"]["current_index"], 0);
  EXPECT_EQ(shown["tableInterfaces"]["rows"][0][4], "Raw CAN");

  ASSERT_TRUE(dialog.onIndexChanged("comboProtocol", 2));
  EXPECT_EQ(dialog.interfaceProtocols().at("vcan0"), CanProtocol::kNmea2000);
  shown = json::parse(dialog.widget_data());
  EXPECT_EQ(shown["comboProtocol"]["current_index"], 2);
  EXPECT_EQ(shown["tableInterfaces"]["rows"][0][4], "NMEA 2000");

  ASSERT_TRUE(dialog.onIndexChanged("comboProtocol", 0));  // back to raw: the entry goes
  EXPECT_TRUE(dialog.interfaceProtocols().empty());
  EXPECT_FALSE(dialog.onIndexChanged("comboProtocol", 3));
}

TEST(CandumpProtocolImport, Nmea2000DecodesByPgnPerSource) {
  const ImportCapture capture = importN2kLog(json{{"vcan0", "nmea2000"}});
  ASSERT_TRUE(capture.ok) << capture.error;

  // One topic per source; a priority change (frames 1 and 8) does not split one.
  EXPECT_EQ(capture.rows.at("CAN/vcan0/VesselHeading/0x10"), 2u);
  EXPECT_EQ(capture.rows.at("CAN/vcan0/VesselHeading/0x04"), 1u);
  // Fast packets: two complete messages, interleaved, one shorter than the DBC message.
  EXPECT_EQ(capture.rows.at("CAN/vcan0/PositionLike/0x10"), 1u);
  EXPECT_EQ(capture.rows.at("CAN/vcan0/PositionLike/0x04"), 1u);
  // PDU1: the destination is part of the topic.
  EXPECT_EQ(capture.rows.at("CAN/vcan0/ISORequest/0x10/0x23"), 1u);
  // Standard frames still match by id; an unknown PGN falls back to raw bytes.
  EXPECT_EQ(capture.rows.at("CAN/vcan0/StdMsg"), 1u);
  EXPECT_EQ(capture.rows.at("CAN/vcan0/0xDF00010"), 1u);
  // Fast-packet frames never show up as raw bytes, complete or not.
  EXPECT_FALSE(capture.hasTopic("CAN/vcan0/0xDF80510"));
  EXPECT_EQ(capture.rows.size(), 7u);

  EXPECT_TRUE(capture.hasMessage(
      PJ_DATA_SOURCE_MESSAGE_INFO,
      "Decoded 7 CAN frame(s) into 6 message topic(s) (2 of them NMEA 2000 fast-packet messages"));
  EXPECT_TRUE(capture.hasMessage(PJ_DATA_SOURCE_MESSAGE_INFO, "2 NMEA 2000 fast-packet frame(s) dropped"));
  // LateStart reassembles, but its 1-byte payload ends before the only signal: undecodable, not a decoded message.
  EXPECT_TRUE(capture.hasMessage(PJ_DATA_SOURCE_MESSAGE_INFO, "1 matched frame(s) could not be decoded"));

  // A fast packet is recorded at its first frame's time: 2 ms after its source's first VesselHeading
  // (frames 3 and 1 for source 0x10, frames 4 and 2 for 0x04), not at its last frame's (frames 7 and 6).
  const auto& heading_10 = capture.timestamps.at("CAN/vcan0/VesselHeading/0x10");
  const auto& heading_04 = capture.timestamps.at("CAN/vcan0/VesselHeading/0x04");
  EXPECT_EQ(capture.timestamps.at("CAN/vcan0/PositionLike/0x10").front() - heading_10.front(), 2'000'000);
  EXPECT_EQ(capture.timestamps.at("CAN/vcan0/PositionLike/0x04").front() - heading_04.front(), 2'000'000);
}

TEST(CandumpProtocolImport, J1939MatchesByPgnWithoutFastPackets) {
  const ImportCapture capture = importN2kLog(json{{"vcan0", "j1939"}});
  ASSERT_TRUE(capture.ok) << capture.error;
  EXPECT_EQ(capture.rows.at("CAN/vcan0/VesselHeading/0x10"), 2u);
  EXPECT_EQ(capture.rows.at("CAN/vcan0/ISORequest/0x10/0x23"), 1u);
  // J1939 has no fast packets: each 8-byte frame is too short for the 20-byte message.
  EXPECT_FALSE(capture.hasTopic("CAN/vcan0/PositionLike/0x10"));
  EXPECT_TRUE(capture.hasMessage(PJ_DATA_SOURCE_MESSAGE_INFO, "8 matched frame(s) could not be decoded"));
}

TEST(CandumpProtocolImport, RawNeedsTheExactId) {
  // The DBC's ids carry source address 0xFE; the capture's frames come from 0x04 and 0x10.
  const ImportCapture capture = importN2kLog(json());
  ASSERT_TRUE(capture.ok) << capture.error;
  EXPECT_FALSE(capture.hasTopic("CAN/vcan0/VesselHeading/0x10"));
  EXPECT_FALSE(capture.hasTopic("CAN/vcan0/VesselHeading"));
  EXPECT_EQ(capture.rows.at("CAN/vcan0/StdMsg"), 1u);
}

}  // namespace
