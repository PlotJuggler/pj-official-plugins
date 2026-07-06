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

}  // namespace
