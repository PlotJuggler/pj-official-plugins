#pragma once

#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>

#include "json_manifest.hpp"
#include "json_parser_options_ui.hpp"

namespace {

/// Name of the embedded timestamp field; an empty or blank name means the
/// default, since the parser needs a concrete key to look up.
std::string timestampFieldNameOrDefault(std::string_view name) {
  const auto first = name.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return "timestamp";
  }
  const auto last = name.find_last_not_of(" \t\r\n");
  return std::string(name.substr(first, last - first + 1));
}

/// Dialog plugin for the JSON Parser options.
/// Allows users to configure embedded timestamp extraction.
class JsonParserDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

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

    // Embedded timestamp controls
    wd.setChecked("checkBoxUseEmbeddedTimestamp", use_embedded_timestamp_);
    wd.setText("lineEditTimestampField", timestamp_field_name_);
    wd.setEnabled("lineEditTimestampField", use_embedded_timestamp_);
    wd.setEnabled("labelTimestampField", use_embedded_timestamp_);

    // Label-keyed arrays checkbox
    wd.setChecked("checkBoxLabelKeyedArrays", label_keyed_arrays_);
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
      return true;  // refresh to enable/disable lineEditTimestampField
    }
    if (widget_name == "checkBoxLabelKeyedArrays") {
      label_keyed_arrays_ = checked;
      return false;
    }
    if (checked && widget_name == "radioMaxClamp") {
      array_limit_.policy = PJ::sdk::ArrayPolicy::kClamp;
      return false;
    }
    if (checked && widget_name == "radioMaxDiscard") {
      array_limit_.policy = PJ::sdk::ArrayPolicy::kSkip;
      return false;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditTimestampField") {
      timestamp_field_name_ = timestampFieldNameOrDefault(text);
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["timestamp_field_name"] = timestamp_field_name_;
    cfg["label_keyed_arrays"] = label_keyed_arrays_;
    PJ::sdk::arrayLimitToJson(cfg, array_limit_);
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    timestamp_field_name_ = timestampFieldNameOrDefault(cfg.value("timestamp_field_name", std::string{}));
    label_keyed_arrays_ = cfg.value("label_keyed_arrays", false);
    array_limit_ = PJ::sdk::arrayLimitFromJson(cfg);
    return true;
  }

 private:
  PJ::sdk::ArrayLimit array_limit_;
  bool use_embedded_timestamp_ = false;
  bool label_keyed_arrays_ = false;
  std::string timestamp_field_name_ = "timestamp";
};

}  // namespace
