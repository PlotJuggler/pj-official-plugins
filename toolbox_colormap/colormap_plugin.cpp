#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "colormap_manifest.hpp"
#include "colormap_dialog_ui.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Trash can icon (Feather Icons, MIT license)
static constexpr const char* kTrashSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24")"
    R"( fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)"
    R"(<polyline points="3 6 5 6 21 6"/>)"
    R"(<path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>)"
    R"(<line x1="10" y1="11" x2="10" y2="17"/>)"
    R"(<line x1="14" y1="11" x2="14" y2="17"/>)"
    R"(</svg>)";

// ---------------------------------------------------------------------------
// ColormapDialog
// ---------------------------------------------------------------------------

class ColormapDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"name":"ColorMap Editor","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kColormapDialogUi; }

  std::string widget_data() override {
    std::vector<std::string> names;
    names.reserve(saved_maps_.size());
    for (const auto& m : saved_maps_) {
      names.push_back(m.name);
    }

    PJ::WidgetData wd;
    wd.setPlainText("code_editor", lua_body_)
        .setText("name_edit", current_name_)
        .setListItems("saved_list", names)
        .setText("status_label", status_msg_)
        .setButtonIcon("delete_btn", kTrashSvg);

    return wd.toJson();
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "code_editor") {
      lua_body_ = std::string(text);
      status_msg_.clear();
      return true;
    }
    if (name == "name_edit") {
      current_name_ = std::string(text);
      return false;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "save_btn") {
      if (current_name_.empty()) {
        status_msg_ = "Enter a name before saving.";
        return true;
      }
      auto it = std::find_if(saved_maps_.begin(), saved_maps_.end(),
                             [&](const SavedMap& m) { return m.name == current_name_; });
      if (it != saved_maps_.end()) {
        it->body = lua_body_;
      } else {
        saved_maps_.push_back({current_name_, lua_body_});
      }
      status_msg_.clear();
      return true;
    }
    if (name == "delete_btn") {
      if (!selected_name_.empty()) {
        saved_maps_.erase(
            std::remove_if(saved_maps_.begin(), saved_maps_.end(),
                           [&](const SavedMap& m) { return m.name == selected_name_; }),
            saved_maps_.end());
        selected_name_.clear();
        status_msg_.clear();
        return true;
      }
      return false;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view name,
                          const std::vector<std::string>& items) override {
    if (name == "saved_list" && !items.empty()) {
      selected_name_ = items.front();
      return false;
    }
    return false;
  }

  bool onItemDoubleClicked(std::string_view name, int index) override {
    if (name == "saved_list" && index >= 0 && index < static_cast<int>(saved_maps_.size())) {
      const auto& map = saved_maps_[static_cast<size_t>(index)];
      lua_body_ = map.body;
      current_name_ = map.name;
      selected_name_ = map.name;
      status_msg_.clear();
      return true;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json j;
    j["lua_body"] = lua_body_;
    nlohmann::json saved = nlohmann::json::array();
    for (const auto& m : saved_maps_) {
      saved.push_back({{"name", m.name}, {"body", m.body}});
    }
    j["saved"] = saved;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) return false;
    lua_body_ = j.value("lua_body", std::string{});
    saved_maps_.clear();
    if (j.contains("saved") && j["saved"].is_array()) {
      for (const auto& item : j["saved"]) {
        saved_maps_.push_back({item.value("name", ""), item.value("body", "")});
      }
    }
    status_msg_.clear();
    selected_name_.clear();
    current_name_.clear();
    return true;
  }

 private:
  struct SavedMap {
    std::string name;
    std::string body;
  };

  std::string lua_body_;
  std::string current_name_;
  std::string selected_name_;
  std::string status_msg_;
  std::vector<SavedMap> saved_maps_;
};

// ---------------------------------------------------------------------------
// ColormapToolbox
// ---------------------------------------------------------------------------

class ColormapToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override { return PJ::kToolboxCapabilityHasDialog; }

  void* dialogContext() override { return &dialog_; }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

 private:
  ColormapDialog dialog_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(ColormapToolbox, kColormapManifest)
PJ_DIALOG_PLUGIN(ColormapDialog)
