#include "can_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using can_stream::CanDialog;
using can_stream::TimestampSource;
using json = nlohmann::json;

CanDialog makeDialog() {
  return CanDialog(
      [] { return std::vector<std::string>{"socketcan", "virtualcan"}; },
      [](const std::string&) -> PJ::Expected<std::vector<std::string>> {
        return std::vector<std::string>{"can0", "vcan0"};
      });
}

const json kFullConfig = {
    {"backend", "socketcan"},
    {"interface", "can0"},
    {"bitrate", 500000},
    {"can_fd", true},
    {"data_bitrate", 2000000},
    {"timestamps", "host"},
    {"dictionaries", {"/abs/path/powertrain.dbc", "/abs/b.csv"}},
    {"raw_unassigned", false},
};

TEST(CanDialog, ConfigRoundTrip) {
  auto dialog = makeDialog();
  ASSERT_TRUE(dialog.loadConfig(kFullConfig.dump()));
  EXPECT_EQ(json::parse(dialog.saveConfig()), kFullConfig);

  const auto settings = dialog.busSettings();
  EXPECT_EQ(settings.backend, "socketcan");
  EXPECT_EQ(settings.interface_name, "can0");
  EXPECT_EQ(settings.bitrate, 500000);
  EXPECT_TRUE(settings.can_fd);
  EXPECT_EQ(settings.data_bitrate, 2000000);
  EXPECT_EQ(settings.timestamps, TimestampSource::kHost);
  EXPECT_EQ(dialog.dictionaries(), (std::vector<std::string>{"/abs/path/powertrain.dbc", "/abs/b.csv"}));
  EXPECT_FALSE(dialog.rawUnassigned());
}

TEST(CanDialog, MalformedConfigKeepsDefaults) {
  auto dialog = makeDialog();
  EXPECT_TRUE(dialog.loadConfig(R"({"bitrate":"fast","dictionaries":7,"timestamps":"bogus"})"));
  const auto settings = dialog.busSettings();
  EXPECT_EQ(settings.bitrate, 0);
  EXPECT_TRUE(dialog.dictionaries().empty());
  EXPECT_EQ(settings.timestamps, TimestampSource::kDevice);
  EXPECT_TRUE(dialog.rawUnassigned());
  EXPECT_NO_THROW((void)dialog.widget_data());
  EXPECT_FALSE(dialog.loadConfig("{not json"));
}

TEST(CanDialog, SavedBackendNotInstalledHereIsKept) {
  auto dialog = makeDialog();
  ASSERT_TRUE(dialog.loadConfig(R"({"backend":"vectorcan","interface":"can0"})"));
  // Rendering shows it as the current backend rather than replacing it with an installed one.
  const auto combo = json::parse(dialog.widget_data())["comboBackend"];
  EXPECT_EQ(combo["items"], json::array({"socketcan", "virtualcan", "vectorcan"}));
  EXPECT_EQ(combo["current_index"], 2);
  EXPECT_EQ(dialog.busSettings().backend, "vectorcan");
  EXPECT_EQ(json::parse(dialog.saveConfig()).value("backend", ""), "vectorcan");
}

TEST(CanDialog, DictionaryListEditing) {
  auto dialog = makeDialog();
  EXPECT_TRUE(dialog.onFileSelected("buttonAddDictionary", "/a.dbc"));
  EXPECT_TRUE(dialog.onFileSelected("buttonAddDictionary", "/b.csv"));
  EXPECT_TRUE(dialog.onFileSelected("buttonAddDictionary", "/a.dbc"));  // no duplicate
  EXPECT_EQ(dialog.dictionaries(), (std::vector<std::string>{"/a.dbc", "/b.csv"}));
  EXPECT_TRUE(dialog.onSelectionChanged("listDictionaries", {"/a.dbc"}));
  EXPECT_TRUE(dialog.onClicked("buttonRemoveDictionary"));
  EXPECT_EQ(dialog.dictionaries(), (std::vector<std::string>{"/b.csv"}));
}

TEST(CanDialog, WidgetEditsReachBusSettings) {
  auto dialog = makeDialog();
  ASSERT_TRUE(dialog.loadConfig("{}"));
  (void)dialog.widget_data();
  EXPECT_EQ(dialog.busSettings().backend, "socketcan");  // first installed backend by default
  dialog.onIndexChanged("comboBackend", 1);
  dialog.onTextChanged("comboInterface", "vcan0");
  dialog.onIndexChanged("comboBitrate", 3);
  dialog.onToggled("checkCanFd", true);
  dialog.onIndexChanged("comboDataBitrate", 2);
  dialog.onIndexChanged("comboTimestamps", 1);
  const auto settings = dialog.busSettings();
  EXPECT_EQ(settings.backend, "virtualcan");
  EXPECT_EQ(settings.interface_name, "vcan0");
  EXPECT_EQ(settings.bitrate, 500000);
  EXPECT_TRUE(settings.can_fd);
  EXPECT_EQ(settings.data_bitrate, 5000000);
  EXPECT_EQ(settings.timestamps, TimestampSource::kHost);
}

}  // namespace
