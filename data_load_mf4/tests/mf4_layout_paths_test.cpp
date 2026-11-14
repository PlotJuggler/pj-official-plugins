// Layout folders that move or get copied: the DBC saved beside the data must be
// the one that decodes after the move/copy, and a missing DBC must be reported
// as missing. Drives the BUILT mf4_source_plugin (import) and Mf4Dialog (config);
// the shared scenarios live in source_import_harness.hpp.
#include <gtest/gtest.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../mf4_dialog.hpp"
#include "mf4_can_fixture.hpp"
#include "source_import_harness.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using pj_can_dbc::testing::generic;

class Mf4LayoutPaths : public pj_can_dbc::testing::LayoutPathsFixture {
 protected:
  const pj_can_dbc::testing::LayoutPathsSpec& spec() const override {
    static const pj_can_dbc::testing::LayoutPathsSpec kSpec{
        MF4_SOURCE_PLUGIN_PATH,
        "run.mf4",
        "CAN/ch1/",
        8,
        [](const fs::path& data) { mf4_test::writeCanBusMf4(generic(data)); },
        [](const fs::path& data, const json& dict_entries, const json& extra) {
          json cfg = extra;
          cfg["filepath"] = generic(data);
          cfg["dbc_paths"] = dict_entries;
          return cfg.dump();
        },
        [](const json& saved) { return saved["dbc_paths"]; }};
    return kSpec;
  }
};

TEST_F(Mf4LayoutPaths, SaveConfigStoresDbcBesideDataRelative) {
  checkSaveStoresRelative<mf4_detail::Mf4Dialog>();

  // MF4-only: with no dictionaries the list stays (an empty array) and no base directory is written.
  mf4_detail::Mf4Dialog empty;
  ASSERT_TRUE(empty.loadConfig(config(old_ / "run.mf4", json::array())));
  const json empty_saved = json::parse(empty.saveConfig());
  EXPECT_EQ(empty_saved["dbc_paths"], json::array());
  EXPECT_FALSE(empty_saved.contains(pj_can_dbc::kAuxBaseDirKey));
}

TEST_F(Mf4LayoutPaths, MovedLayoutDecodesWithDbcBesideNewData) {
  checkMovedLayoutDecodesBesideNewData();
}

TEST_F(Mf4LayoutPaths, MovedLegacyAbsoluteConfigIsRebased) {
  checkMovedLegacyAbsoluteConfigIsRebased();
}

TEST_F(Mf4LayoutPaths, CopiedLayoutUsesTheCopysDbc) {
  checkCopiedLayoutUsesTheCopysDbc();
}

TEST_F(Mf4LayoutPaths, SiblingDictionaryFolderFollowsMove) {
  checkMovedLegacyAbsoluteConfigIsRebased("data", "dbc");
}

TEST_F(Mf4LayoutPaths, SiblingDictionaryFolderFollowsCopy) {
  checkCopiedLayoutUsesTheCopysDbc("data", "dbc");
}

TEST_F(Mf4LayoutPaths, MissingDbcReportsNotFoundWarning) {
  checkMissingDbcReportsNotFoundWarning();
}

TEST_F(Mf4LayoutPaths, PrefillForAnotherDataFileKeepsSavedBase) {
  checkPrefillForAnotherDataFileKeepsSavedBase<mf4_detail::Mf4Dialog>(
      [](const mf4_detail::Mf4Dialog& dialog) { return dialog.dbcPaths(); });
}

}  // namespace
