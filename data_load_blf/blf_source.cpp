#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>

#include "blf_manifest.hpp"

namespace {

// Phase 2 scaffold: a minimal FileSourceBase so the plugin links against lblf
// and the shared pj_can_dbc decoder. The BLF-frame adapter and the real
// importData() (decode frames via DBC -> timeseries) land in Phases 3-4.
class BlfSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
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
    // Full BLF import lands in Phase 4.
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(BlfSource, kBlfManifest)
