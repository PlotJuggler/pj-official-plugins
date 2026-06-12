#pragma once

#include <nlohmann/json.hpp>
#include <pj_array_policy/array_policy.hpp>
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

    // Array-size policy
    wd.setValue("spinBoxArraySize", static_cast<int>(array_limit_.max_size));
    wd.setChecked("radioMaxClamp", array_limit_.clamp());
    wd.setChecked("radioMaxDiscard", !array_limit_.clamp());

    // Embedded timestamp checkbox
    wd.setChecked("checkBoxUseEmbeddedTimestamp", use_embedded_timestamp_);
    return wd.toJson();
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBoxArraySize") {
      array_limit_.max_size = static_cast<uint32_t>(value);
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxUseEmbeddedTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    if (checked && widget_name == "radioMaxClamp") {
      array_limit_.policy = pj::array_policy::ArrayPolicy::kClamp;
      return false;
    }
    if (checked && widget_name == "radioMaxDiscard") {
      array_limit_.policy = pj::array_policy::ArrayPolicy::kSkip;
      return false;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["timestamp_field_name"] = timestamp_field_name_;
    pj::array_policy::arrayLimitToJson(cfg, array_limit_);
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = cfg.value("timestamp_field_name", std::string("timestamp"));
    array_limit_ = pj::array_policy::arrayLimitFromJson(cfg);
    return true;
  }

 private:
  pj::array_policy::ArrayLimit array_limit_;
  bool use_embedded_timestamp_ = false;
  std::string timestamp_field_name_ = "timestamp";
};

}  // namespace
