#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mcap/writer.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace {

TEST(McapDialogTest, DefaultConstructs) {
  McapDialog dialog;
  EXPECT_TRUE(dialog.selectedTopics().empty());
  EXPECT_TRUE(dialog.analyzeError().empty());
}

TEST(McapDialogTest, WidgetDataSelectsRowsByTextNotIndex) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("tableWidget"));
  const auto& tbl = data["tableWidget"];

  ASSERT_TRUE(tbl.contains("selected_items"));
  std::vector<std::string> selected_items = tbl["selected_items"].get<std::vector<std::string>>();
  EXPECT_EQ(selected_items, (std::vector<std::string>{"/sensor/value"}));

  EXPECT_FALSE(tbl.contains("selected_rows"));
}

TEST(McapDialogTest, DropsSelectionsThatDoNotExistInNewRecording) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  cfg["selected_topics"] = std::vector<std::string>{"/topic/from/previous/file"};

  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  EXPECT_EQ(dialog.selectedTopics(), (std::unordered_set<std::string>{"/sensor/value"}));
  const auto data = nlohmann::json::parse(dialog.widget_data());
  EXPECT_TRUE(data["buttonBox"]["ok_enabled"].get<bool>());
}

TEST(AlwaysIncludeRuleTest, MatchesRos2Tf) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, MatchesFoxgloveFrameTransformUnderAnyTopicName) {
  ChannelInfo ch;
  ch.topic = "transforms";  // deliberately not "/tf"
  ch.schema = "foxglove.FrameTransform";
  ch.encoding = "protobuf";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightSchemaWrongEncoding) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "json";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightEncodingWrongSchema) {
  ChannelInfo ch;
  ch.topic = "/sensor/value";
  ch.schema = "std_msgs/Float64";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}

TEST(McapDialogTest, ForceIncludesOnlyWhitelistedNonEmptyChannels) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  // Saved selection predates this feature and omits everything except one
  // ordinary topic -- as if loading an old layout.
  cfg["selected_topics"] = std::vector<std::string>{"/sensor/value2"};

  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  // Pin the fixture: all five channels must be present in the analyzed table,
  // otherwise the negative assertions below would pass vacuously.
  const auto data = nlohmann::json::parse(dialog.widget_data());
  std::set<std::string> row_topics;
  for (const auto& row : data["tableWidget"]["rows"]) {
    row_topics.insert(row[0].get<std::string>());
  }
  EXPECT_EQ(
      row_topics, (std::set<std::string>{"/near_miss_encoding", "/sensor/value2", "/tf", "/tf_static", "transforms"}));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);                // whitelisted, has messages -> forced back in
  EXPECT_TRUE(selected.count("transforms") > 0);         // whitelisted (any topic name) -> forced back in
  EXPECT_EQ(selected.count("/tf_static"), 0u);           // whitelisted schema/encoding, zero messages -> NOT forced
  EXPECT_EQ(selected.count("/near_miss_encoding"), 0u);  // right schema, wrong encoding -> NOT forced
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);     // explicitly saved by the user -> stays
}

TEST(McapDialogTest, WhitelistedRowsAreDisabledAndTooltippedInWidgetData) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  const auto& rows = data["tableWidget"]["rows"];

  int tf_row = -1;
  int ordinary_row = -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i][0] == "/tf") {
      tf_row = static_cast<int>(i);
    }
    if (rows[i][0] == "/sensor/value2") {
      ordinary_row = static_cast<int>(i);
    }
  }
  ASSERT_NE(tf_row, -1);
  ASSERT_NE(ordinary_row, -1);

  const auto& disabled = data["tableWidget"]["disabled_rows"];
  EXPECT_NE(std::find(disabled.begin(), disabled.end(), tf_row), disabled.end());
  EXPECT_EQ(std::find(disabled.begin(), disabled.end(), ordinary_row), disabled.end());

  ASSERT_TRUE(data["tableWidget"].contains("cell_tooltips"));
  const auto& tooltips = data["tableWidget"]["cell_tooltips"];
  EXPECT_TRUE(tooltips.contains(std::to_string(tf_row) + ",0"));
}

TEST(McapDialogTest, DeselectAllKeepsWhitelistedTopicsSelected) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  EXPECT_TRUE(dialog.onClicked("btnDeselectAll"));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_EQ(selected.count("/sensor/value2"), 0u);
}

TEST(McapDialogTest, HostReportedSelectionOmittingWhitelistIsOverridden) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  // Simulate the host reporting a selection that omits /tf and transforms --
  // the dialog must add them back regardless.
  EXPECT_TRUE(dialog.onSelectionChanged("tableWidget", {"/sensor/value2"}));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);
}

// ---------------------------------------------------------------------------
// Analysis failures must be visible.
//
// A file the reader cannot open leaves the picker with no rows and OK
// disabled. Without an explanation that reads as "this recording has no
// topics" rather than "this file is unreadable".
// ---------------------------------------------------------------------------

// Writes a file with a deliberately corrupt leading magic. McapReader::open()
// rejects it up front, which is the failure the dialog has to explain.
std::string writeUnopenableFile() {
  const auto path = std::filesystem::temp_directory_path() / "pj_mcap_dialog_unopenable.mcap";
  std::ofstream out(path, std::ios::binary);
  const char junk[] =
      "\x89NOTMCAP\r\n this is not an MCAP recording at all, but it is long enough "
      "to clear the reader's minimum-size check for a header plus a footer tail.";
  out.write(junk, sizeof(junk) - 1);
  out.close();
  return path.string();
}

TEST(McapDialogTest, AnalyzeErrorIsSetWhenTheFileCannotBeOpened) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = writeUnopenableFile();
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  EXPECT_FALSE(dialog.analyzeError().empty()) << "an unreadable file must record why";
  EXPECT_TRUE(dialog.selectedTopics().empty());
}

TEST(McapDialogTest, AnalyzeErrorIsSurfacedInWidgetData) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = writeUnopenableFile();
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("labelAnalyzeError"));
  EXPECT_TRUE(data["labelAnalyzeError"].value("visible", false));
  // The host applies QLabel content from the "label" key, not "text".
  EXPECT_FALSE(data["labelAnalyzeError"].value("label", std::string{}).empty());
}

// A restored config carries the previous recording's topic selection. If
// analysis fails we never reach the pruning step, so those stale names would
// survive and leave OK enabled over an empty table — clicking it would start
// an import that cannot bind anything.
TEST(McapDialogTest, AnalysisFailureClearsStaleSelectionAndDisablesOk) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = writeUnopenableFile();
  cfg["selected_topics"] = std::vector<std::string>{"/topic/from/previous/file"};
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  ASSERT_FALSE(dialog.analyzeError().empty());
  EXPECT_TRUE(dialog.selectedTopics().empty()) << "stale selection must not survive a failed analysis";

  const auto data = nlohmann::json::parse(dialog.widget_data());
  EXPECT_FALSE(data["buttonBox"]["ok_enabled"].get<bool>());
}

TEST(McapDialogTest, AnalyzeErrorLabelStaysHiddenForAHealthyRecording) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  ASSERT_TRUE(dialog.analyzeError().empty());
  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("labelAnalyzeError"));
  EXPECT_FALSE(data["labelAnalyzeError"].value("visible", true));
}

// The encoding combo box was removed from dialog_mcap.ui because message
// encoding is a per-channel property, not a dialog-wide choice. Widget data
// naming widgets the .ui does not contain is silently dropped by the host, so
// nothing catches the leftovers except a test.
TEST(McapDialogTest, WidgetDataNamesNoWidgetAbsentFromTheUi) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const std::string ui(kDialogMcapUi);
  const auto data = nlohmann::json::parse(dialog.widget_data());
  for (const auto& [widget_name, value] : data.items()) {
    if (widget_name.rfind("__", 0) == 0 || widget_name == "ok_enabled") {
      continue;  // dialog-level directives, not widget names
    }
    // A widget is real if the .ui declares it directly, or if a
    // SectionHeaderBand synthesizes it (its filterFieldName names the filter
    // QLineEdit the host creates inside the band). Both halves must come from
    // the SAME property: testing that "filterFieldName" occurs anywhere in the
    // file would let a coincidental <string> — a label's text, say — vouch for
    // a widget_data key that targets nothing.
    const bool declared = ui.find("name=\"" + widget_name + "\"") != std::string::npos;
    bool synthesized_filter_field = false;
    const std::string value_tag = "<string>" + widget_name + "</string>";
    // <property name="filterFieldName"> and its <string> value sit within a
    // line or two of each other; anything further away is a later property.
    constexpr size_t kPropertyValueWindow = 80;
    for (size_t at = ui.find("filterFieldName"); at != std::string::npos; at = ui.find("filterFieldName", at + 1)) {
      const size_t value_at = ui.find(value_tag, at);
      if (value_at != std::string::npos && (value_at - at) < kPropertyValueWindow) {
        synthesized_filter_field = true;
        break;
      }
    }
    EXPECT_TRUE(declared || synthesized_filter_field)
        << "widget_data targets '" << widget_name << "' but dialog_mcap.ui has no such widget";
  }
}

// schema_id 0 is the spec's "no schema" sentinel, not a broken reference:
// schemaless JSON recordings use it routinely. Such a channel must still reach
// the picker — a topic that cannot be selected can never be imported, so
// dropping it here silently hides an entire valid class of file.
TEST(McapDialogTest, SchemalessChannelsAreOfferedInThePicker) {
  const auto path = std::filesystem::temp_directory_path() / "pj_mcap_schemaless_dialog_test.mcap";

  {
    mcap::McapWriter writer;
    mcap::McapWriterOptions opts("test");
    ASSERT_TRUE(writer.open(path.string(), opts).ok());

    mcap::Schema schema("std_msgs/msg/String", "ros2msg", "string data");
    writer.addSchema(schema);

    mcap::Channel with_schema("/with_schema", "cdr", schema.id);
    writer.addChannel(with_schema);

    // The channel under test: no schema, format carried by messageEncoding.
    mcap::Channel schemaless("/schemaless_json", "json", 0);
    writer.addChannel(schemaless);

    const std::string payload = "{\"value\":1}";
    const mcap::ChannelId channel_ids[] = {with_schema.id, schemaless.id};
    for (mcap::ChannelId cid : channel_ids) {
      mcap::Message msg;
      msg.channelId = cid;
      msg.sequence = 1;
      msg.logTime = 1000;
      msg.publishTime = 1000;
      msg.data = reinterpret_cast<const std::byte*>(payload.data());
      msg.dataSize = payload.size();
      ASSERT_TRUE(writer.write(msg).ok());
    }
    writer.close();
  }

  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = path.string();
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));
  EXPECT_TRUE(dialog.analyzeError().empty()) << dialog.analyzeError();

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("tableWidget"));
  const std::string table = data["tableWidget"].dump();
  EXPECT_NE(table.find("/schemaless_json"), std::string::npos)
      << "schemaless (schema_id 0) channel missing from the picker: " << table;
  EXPECT_NE(table.find("/with_schema"), std::string::npos);

  std::filesystem::remove(path);
}

}  // namespace
