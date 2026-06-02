#pragma once

#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>

#include "json_manifest.hpp"
#include "json_parser_options_ui.hpp"

namespace {

/// Dialog plugin for the JSON Parser options.
/// Allows users to configure embedded timestamp extraction.
class JsonParserDialog : public PJ::DialogPluginTyped {
 public:
  // --- Dialog protocol ---

  std::string manifest() const override {
    return kJsonManifest;
  }

  std::string ui_content() const override {
    return kJsonParserOptionsUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setChecked("checkBoxUseEmbeddedTimestamp", use_embedded_timestamp_);
    return wd.toJson();
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseEmbeddedTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["timestamp_field_name"] = timestamp_field_name_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = cfg.value("timestamp_field_name", std::string("timestamp"));
    return true;
  }

 private:
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_ = "timestamp";
};

}  // namespace
