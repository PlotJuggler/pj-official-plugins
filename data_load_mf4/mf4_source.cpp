#include <mdf/mdfreader.h>

#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>

#include "mf4_manifest.hpp"

namespace {

// Phase 1 scaffold: a minimal FileSourceBase that proves the mdflib integration
// links and can open a file. Metadata enumeration, channel-group import, and
// CAN/DBC decoding land in Phase 5 (see docs/plans/2026-07-01-mf4-plugin-*.md).
class Mf4Source : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }
    mdf::MdfReader reader(filepath_);
    if (!reader.IsOk()) {
      return PJ::unexpected(std::string("cannot open MDF file: ") + filepath_);
    }
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Mf4Source, kMf4Manifest)
