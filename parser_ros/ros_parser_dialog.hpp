#pragma once

#include <nlohmann/json.hpp>
#include <pj_array_policy/array_policy.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>

#include "ros_manifest.hpp"
#include "ros_parser_options_ui.hpp"

namespace {

class RosParserDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kRosManifest;
  }

  std::string ui_content() const override {
    return kRosParserOptionsUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setValue("spinBoxArraySize", static_cast<int>(max_array_size_));
    wd.setChecked("radioMaxClamp", !discard_large_arrays_);
    wd.setChecked("radioMaxDiscard", discard_large_arrays_);
    wd.setChecked("checkBoxTimestamp", use_embedded_timestamp_);
    wd.setChecked("checkBoxStringBoolean", boolean_strings_to_number_);
    wd.setChecked("checkBoxStringSuffix", remove_suffix_from_strings_);
    return wd.toJson();
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBoxArraySize") {
      max_array_size_ = value;
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    if (widget_name == "checkBoxStringBoolean") {
      boolean_strings_to_number_ = checked;
      return false;
    }
    if (widget_name == "checkBoxStringSuffix") {
      remove_suffix_from_strings_ = checked;
      return false;
    }
    if (!checked) {
      return false;
    }
    if (widget_name == "radioMaxClamp") {
      discard_large_arrays_ = false;
      return false;
    }
    if (widget_name == "radioMaxDiscard") {
      discard_large_arrays_ = true;
      return false;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    pj::array_policy::arrayLimitToJson(cfg, static_cast<uint32_t>(max_array_size_), !discard_large_arrays_);
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["boolean_strings_to_number"] = boolean_strings_to_number_;
    cfg["remove_suffix_from_strings"] = remove_suffix_from_strings_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    const auto array_limit = pj::array_policy::arrayLimitFromJson(cfg);
    max_array_size_ = static_cast<int>(array_limit.max_size);
    discard_large_arrays_ = (array_limit.policy == pj::array_policy::ArrayPolicy::kSkip);
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    boolean_strings_to_number_ = cfg.value("boolean_strings_to_number", false);
    remove_suffix_from_strings_ = cfg.value("remove_suffix_from_strings", false);
    return true;
  }

 private:
  int max_array_size_ = 500;
  bool discard_large_arrays_ = false;
  bool use_embedded_timestamp_ = false;
  bool boolean_strings_to_number_ = false;
  bool remove_suffix_from_strings_ = false;
};

}  // namespace
