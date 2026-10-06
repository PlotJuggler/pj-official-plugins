#include "../json_parser_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

TEST(JsonParserDialogTest, TextChangeUpdatesSavedTimestampFieldName) {
  JsonParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(nlohmann::json{{"use_embedded_timestamp", true}}.dump()));

  dialog.onTextChanged("lineEditTimestampField", "ts");

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["timestamp_field_name"], "ts");
}

TEST(JsonParserDialogTest, TextChangeTrimsWhitespace) {
  JsonParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(nlohmann::json{{"use_embedded_timestamp", true}}.dump()));

  dialog.onTextChanged("lineEditTimestampField", "  my_ts  ");

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["timestamp_field_name"], "my_ts");
}

TEST(JsonParserDialogTest, TextChangeEmptyFallsBackToDefaultTimestampName) {
  JsonParserDialog dialog;
  ASSERT_TRUE(
      dialog.loadConfig(nlohmann::json{{"use_embedded_timestamp", true}, {"timestamp_field_name", "ts"}}.dump()));

  dialog.onTextChanged("lineEditTimestampField", "   ");

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["timestamp_field_name"], "timestamp");
}

TEST(JsonParserDialogTest, TimestampFieldDisabledWhenEmbeddedTimestampOff) {
  JsonParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(nlohmann::json{{"use_embedded_timestamp", false}}.dump()));

  const auto widget_data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(widget_data.contains("lineEditTimestampField"));
  EXPECT_EQ(widget_data["lineEditTimestampField"]["enabled"], false);
}

TEST(JsonParserDialogTest, TimestampFieldEnabledWhenEmbeddedTimestampOn) {
  JsonParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(nlohmann::json{{"use_embedded_timestamp", true}}.dump()));

  const auto widget_data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(widget_data.contains("lineEditTimestampField"));
  EXPECT_EQ(widget_data["lineEditTimestampField"]["enabled"], true);
}

TEST(JsonParserDialogTest, CheckboxToggleRequestsRefresh) {
  JsonParserDialog dialog;
  EXPECT_TRUE(dialog.onToggled("checkBoxUseEmbeddedTimestamp", true));
}

}  // namespace
