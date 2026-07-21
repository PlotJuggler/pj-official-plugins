#include "../mf4_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

namespace {

using mf4_detail::Mf4Dialog;

// The host round-trips the accepted dialog's saveConfig() back into the
// source's loadConfig() (PJ4 FileLoader post-dialog reload). The dialog must
// therefore carry every source setting it does not edit — dropping dbc_paths
// would silently disable DBC decoding on every interactive open.
TEST(Mf4Dialog, SaveConfigRoundTripsDbcPaths) {
  Mf4Dialog dialog;
  const std::string config = nlohmann::json{{"filepath", ""}, {"dbc_paths", {"powertrain.dbc", "diag.dbc"}}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved.value("dbc_paths", nlohmann::json::array()), nlohmann::json::array({"powertrain.dbc", "diag.dbc"}));
}

// The source primes its borrowed dialog programmatically (the DialogEngine
// never feeds the source config into the dialog), so the setter path must
// reach saveConfig too.
TEST(Mf4Dialog, SetDbcPathsReflectsInSaveConfig) {
  Mf4Dialog dialog;
  dialog.setDbcPaths({"bus.dbc"});

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved.value("dbc_paths", nlohmann::json::array()), nlohmann::json::array({"bus.dbc"}));
}

TEST(Mf4Dialog, SaveConfigWithoutDbcPathsEmitsEmptyArray) {
  Mf4Dialog dialog;
  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  ASSERT_TRUE(saved.contains("dbc_paths"));
  EXPECT_TRUE(saved["dbc_paths"].is_array());
  EXPECT_TRUE(saved["dbc_paths"].empty());
}

}  // namespace
