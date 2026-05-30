#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

TEST(McapDialogTest, UsesChannelMessageEncodingForParserSlot) {
  McapDialog dialog;
  const std::string cfg =
      nlohmann::json{{"filepath", std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap"}}.dump();

  ASSERT_TRUE(dialog.loadConfig(cfg));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("comboBoxProtocol"));
  ASSERT_TRUE(data["comboBoxProtocol"].contains("items"));

  const auto& items = data["comboBoxProtocol"]["items"];
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items.at(0), "cdr");
}

TEST(McapDialogTest, ParserSlotHasHideableContainer) {
  McapDialog dialog;
  const std::string ui = dialog.ui_content();

  EXPECT_NE(ui.find("parserSlotContainer"), std::string::npos);
  EXPECT_NE(ui.find("pj_parser_slot"), std::string::npos);
}

}  // namespace
