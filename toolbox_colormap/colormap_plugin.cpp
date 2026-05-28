#include <algorithm>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sol/sol.hpp>
#include <string>
#include <vector>

#include "colormap_dialog_ui.hpp"
#include "colormap_manifest.hpp"

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
// LuaColorMap — compiles and evaluates a Lua ColorMap(v) function.
// Mirrors the PJ 3.x ColorMap class but lives entirely in the plugin.
// ---------------------------------------------------------------------------

struct LuaColorMap {
  sol::state lua;
  sol::protected_function func;
  std::string last_result;  // buffer for the returned color string

  bool compile(const std::string& body) {
    lua = {};
    func = {};
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string);
    auto wrapped = "function ColorMap(v)\n" + body + "\nend\n";
    auto result = lua.safe_script(wrapped, sol::script_pass_on_error);
    if (!result.valid()) {
      return false;
    }
    func = lua.get<sol::protected_function>("ColorMap");
    return true;
  }

  /// Evaluate the colormap for a scalar value. Returns a CSS color string
  /// or empty string on error. The returned c_str() is valid until the next call.
  const char* evaluate(double value) {
    if (!func.valid()) {
      last_result.clear();
      return last_result.c_str();
    }
    auto res = func(value);
    if (res.valid() && res.return_count() == 1 && res.get_type(0) == sol::type::string) {
      last_result = res.get<std::string>(0);
    } else {
      last_result.clear();
    }
    return last_result.c_str();
  }
};

// C callback for the host registry. user_ctx points to a LuaColorMap*.
static const char* colormapEvalCallback(double value, void* user_ctx) {
  auto* cm = static_cast<LuaColorMap*>(user_ctx);
  return cm->evaluate(value);
}

// ---------------------------------------------------------------------------
// ColormapDialog
// ---------------------------------------------------------------------------

class ColormapDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kColormapManifest;
  }

  std::string ui_content() const override {
    return kColormapDialogUi;
  }

  std::string widget_data() override {
    std::vector<std::string> names;
    names.reserve(saved_maps_.size());
    for (const auto& m : saved_maps_) {
      names.push_back(m.name);
    }

    PJ::WidgetData wd;
    wd.setCodeContent("code_editor", lua_body_)
        .setCodeLanguage("code_editor", "lua")
        .setText("name_edit", current_name_)
        .setListItems("saved_list", names)
        .setText("status_label", status_msg_)
        .setButtonIcon("delete_btn", kTrashSvg);

    if (should_accept_) {
      should_accept_ = false;
      wd.requestAccept();
    }

    return wd.toJson();
  }

  bool onCodeChanged(std::string_view name, std::string_view code) override {
    if (name == "code_editor") {
      lua_body_ = std::string(code);
      status_msg_.clear();
      return false;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
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

      // Try to compile the Lua script
      auto cm = std::make_shared<LuaColorMap>();
      if (!cm->compile(lua_body_)) {
        status_msg_ = "Lua compilation error.";
        return true;
      }

      // Update or add to saved maps
      auto it = std::find_if(
          saved_maps_.begin(), saved_maps_.end(), [&](const SavedMap& m) { return m.name == current_name_; });
      if (it != saved_maps_.end()) {
        it->body = lua_body_;
        it->colormap = cm;
      } else {
        saved_maps_.push_back({current_name_, lua_body_, cm});
      }

      // Register with host (if bound)
      if (register_fn_) {
        register_fn_(current_name_, colormapEvalCallback, cm.get());
      }

      status_msg_ = "Saved and registered: " + current_name_;
      return true;
    }
    if (name == "delete_btn") {
      if (!selected_name_.empty()) {
        // Unregister from host
        if (unregister_fn_) {
          unregister_fn_(selected_name_);
        }
        saved_maps_.erase(
            std::remove_if(
                saved_maps_.begin(), saved_maps_.end(), [&](const SavedMap& m) { return m.name == selected_name_; }),
            saved_maps_.end());
        selected_name_.clear();
        status_msg_.clear();
        return true;
      }
      return false;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
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
      should_accept_ = true;
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
    if (j.is_discarded()) {
      return false;
    }
    lua_body_ = j.value("lua_body", std::string{});
    // Unregister before destroying the LuaColorMap objects to avoid dangling
    // pointers in the registry (registry holds raw user_ctx pointers).
    if (unregister_fn_) {
      for (const auto& m : saved_maps_) {
        unregister_fn_(m.name);
      }
    }
    saved_maps_.clear();
    if (j.contains("saved") && j["saved"].is_array()) {
      for (const auto& item : j["saved"]) {
        auto cm = std::make_shared<LuaColorMap>();
        std::string body = item.value("body", "");
        cm->compile(body);
        saved_maps_.push_back({item.value("name", ""), body, cm});
      }
    }
    if (register_fn_) {
      for (auto& m : saved_maps_) {
        if (m.colormap && m.colormap->func.valid()) {
          register_fn_(m.name, colormapEvalCallback, m.colormap.get());
        }
      }
    }
    status_msg_.clear();
    selected_name_.clear();
    current_name_.clear();
    return true;
  }

  /// Called by ColormapToolbox to wire the register/unregister functions.
  using RegisterFn = std::function<bool(const std::string&, const char* (*)(double, void*), void*)>;
  using UnregisterFn = std::function<bool(const std::string&)>;

  void setHostCallbacks(RegisterFn reg, UnregisterFn unreg) {
    register_fn_ = std::move(reg);
    unregister_fn_ = std::move(unreg);
  }

  /// Re-register all saved colormaps (called after host binding).
  void registerAllColormaps() {
    if (!register_fn_) {
      return;
    }
    for (auto& m : saved_maps_) {
      if (m.colormap && m.colormap->func.valid()) {
        register_fn_(m.name, colormapEvalCallback, m.colormap.get());
      }
    }
  }

 private:
  struct SavedMap {
    std::string name;
    std::string body;
    std::shared_ptr<LuaColorMap> colormap;
  };

  std::string lua_body_;
  std::string current_name_;
  std::string selected_name_;
  std::string status_msg_;
  std::vector<SavedMap> saved_maps_;
  bool should_accept_ = false;
  RegisterFn register_fn_;
  UnregisterFn unregister_fn_;
};

// ---------------------------------------------------------------------------
// ColormapToolbox
// ---------------------------------------------------------------------------

class ColormapToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Wire registry callbacks on first access (after bind() acquires the
    // ColorMapRegistry service).
    if (!callbacks_wired_ && colorMapRegistryBound()) {
      auto registry = colorMapRegistry();
      dialog_.setHostCallbacks(
          [registry](const std::string& name, const char* (*fn)(double, void*), void* ctx) -> bool {
            return registry.registerMap(name, fn, ctx).has_value();
          },
          [registry](const std::string& name) -> bool { return registry.unregisterMap(name).has_value(); });
      dialog_.registerAllColormaps();
      callbacks_wired_ = true;
    }
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

 private:
  ColormapDialog dialog_;
  bool callbacks_wired_ = false;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(ColormapToolbox, kColormapManifest)
PJ_DIALOG_PLUGIN(ColormapDialog)
