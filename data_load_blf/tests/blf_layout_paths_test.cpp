// Layout folders that move or get copied: the DBC saved beside the data must be
// the one that decodes after the move/copy, and a missing DBC must be reported
// as missing. Drives the BUILT blf_source_plugin (import) and BlfDialog (config);
// the scenarios live in source_import_harness.hpp.
#include <gtest/gtest.h>

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

#include "../blf_dialog.hpp"
#include "source_import_harness.hpp"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using pj_can_dbc::testing::generic;

class BlfLayoutPaths : public pj_can_dbc::testing::LayoutPathsFixture {
 protected:
  const pj_can_dbc::testing::LayoutPathsSpec& spec() const override {
    static const pj_can_dbc::testing::LayoutPathsSpec kSpec{
        BLF_SOURCE_PLUGIN_PATH,
        "run.blf",
        "CAN/ch1/",
        8,
        [](const fs::path& data) { fs::copy_file(fs::path(BLF_TEST_DATA_DIR) / "sample.blf", data); },
        [](const fs::path& data, const json& dict_entries, const json& extra) {
          json cfg = extra;
          cfg["filepath"] = generic(data);
          cfg["channel_dbcs"] = json::object({{"1", dict_entries}});
          return cfg.dump();
        },
        [](const json& saved) { return saved["channel_dbcs"]["1"]; }};
    return kSpec;
  }
};

TEST_F(BlfLayoutPaths, SaveConfigStoresDbcBesideDataRelative) {
  checkSaveStoresRelative<blf_detail::BlfDialog>();
}

TEST_F(BlfLayoutPaths, MovedLayoutDecodesWithDbcBesideNewData) {
  checkMovedLayoutDecodesBesideNewData();
}

TEST_F(BlfLayoutPaths, MovedLegacyAbsoluteConfigIsRebased) {
  checkMovedLegacyAbsoluteConfigIsRebased();
}

TEST_F(BlfLayoutPaths, CopiedLayoutUsesTheCopysDbc) {
  checkCopiedLayoutUsesTheCopysDbc();
}

TEST_F(BlfLayoutPaths, SiblingDictionaryFolderFollowsMove) {
  checkMovedLegacyAbsoluteConfigIsRebased("data", "dbc");
}

TEST_F(BlfLayoutPaths, SiblingDictionaryFolderFollowsCopy) {
  checkCopiedLayoutUsesTheCopysDbc("data", "dbc");
}

TEST_F(BlfLayoutPaths, MissingDbcReportsNotFoundWarning) {
  checkMissingDbcReportsNotFoundWarning();
}

TEST_F(BlfLayoutPaths, PrefillForAnotherDataFileKeepsSavedBase) {
  checkPrefillForAnotherDataFileKeepsSavedBase<blf_detail::BlfDialog>([](const blf_detail::BlfDialog& dialog) {
    const auto it = dialog.channelDbcs().find(1);
    return it == dialog.channelDbcs().end() ? std::vector<std::string>() : it->second;
  });
}

}  // namespace
