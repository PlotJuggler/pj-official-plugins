// Layout folders that move or get copied: the dictionary saved beside the data
// must be the one that decodes after the move/copy, and a missing dictionary
// must be reported as missing. Drives the BUILT candump_source_plugin (import)
// and CandumpDialog (config); the shared scenarios live in
// source_import_harness.hpp.
#include <gtest/gtest.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../candump_dialog.hpp"
#include "source_import_harness.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using pj_can_dbc::testing::generic;

class CandumpLayoutPaths : public pj_can_dbc::testing::LayoutPathsFixture {
 protected:
  const pj_can_dbc::testing::LayoutPathsSpec& spec() const override {
    static const pj_can_dbc::testing::LayoutPathsSpec kSpec{
        CANDUMP_SOURCE_PLUGIN_PATH,
        "capture.log",
        "CAN/can0/",
        2,
        [](const fs::path& data) { fs::copy_file(fs::path(CANDUMP_TEST_DATA_DIR) / "log_format.log", data); },
        [](const fs::path& data, const json& dict_entries, const json& extra) {
          json cfg = extra;
          cfg["filepath"] = generic(data);
          cfg["iface_dicts"] = json::object({{"can0", dict_entries}});
          return cfg.dump();
        },
        [](const json& saved) { return saved["iface_dicts"]["can0"]; }};
    return kSpec;
  }
};

TEST_F(CandumpLayoutPaths, SaveConfigStoresDictionaryBesideDataRelative) {
  checkSaveStoresRelative<candump_detail::CandumpDialog>();
}

TEST_F(CandumpLayoutPaths, MovedLayoutDecodesWithDictionaryBesideNewData) {
  checkMovedLayoutDecodesBesideNewData();
}

TEST_F(CandumpLayoutPaths, MovedLegacyAbsoluteConfigIsRebased) {
  checkMovedLegacyAbsoluteConfigIsRebased();
}

TEST_F(CandumpLayoutPaths, CopiedLayoutUsesTheCopysDictionary) {
  checkCopiedLayoutUsesTheCopysDbc();
}

TEST_F(CandumpLayoutPaths, SiblingDictionaryFolderFollowsMove) {
  checkMovedLegacyAbsoluteConfigIsRebased("data", "dbc");
}

TEST_F(CandumpLayoutPaths, SiblingDictionaryFolderFollowsCopy) {
  checkCopiedLayoutUsesTheCopysDbc("data", "dbc");
}

TEST_F(CandumpLayoutPaths, MissingDbcReportsNotFoundWarning) {
  checkMissingDbcReportsNotFoundWarning();
}

TEST_F(CandumpLayoutPaths, PrefillForAnotherDataFileKeepsSavedBase) {
  checkPrefillForAnotherDataFileKeepsSavedBase<candump_detail::CandumpDialog>(
      [](const candump_detail::CandumpDialog& dialog) {
        const auto it = dialog.interfaceDicts().find("can0");
        return it == dialog.interfaceDicts().end() ? std::vector<std::string>() : it->second;
      });
}

TEST_F(CandumpLayoutPaths, MissingCsvDictionaryReportsNotFoundWarning) {
  const auto result = runWith(old_ / "capture.log", json::array({"gone.csv"}));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(
      result.hasMessage(PJ_DATA_SOURCE_MESSAGE_WARNING, "dictionary file not found: " + generic(old_ / "gone.csv")));
}

}  // namespace
