#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
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

}  // namespace
