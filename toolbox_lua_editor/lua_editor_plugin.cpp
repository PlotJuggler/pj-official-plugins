#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lua_editor_manifest.hpp"
#include "lua_editor_dialog_ui.hpp"

namespace {

// ---------------------------------------------------------------------------
// SnippetData — stored function in the library
// ---------------------------------------------------------------------------

struct SnippetData {
  std::string code;
  std::string global_code;
  std::string function_name;
};

// ---------------------------------------------------------------------------
// validateLuaSyntax — lightweight parse check (no execution)
// ---------------------------------------------------------------------------

std::string validateLuaSyntax(const std::string& global_code, const std::string& function_code) {
  try {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

    if (!global_code.empty()) {
      auto result = lua.safe_script(global_code, sol::script_pass_on_error);
      if (!result.valid()) {
        sol::error err = result;
        return "Global: " + std::string(err.what());
      }
    }

    std::string wrapped = "function _pj_user_func(tracker_time)\n" + function_code + "\nend";
    auto result = lua.safe_script(wrapped, sol::script_pass_on_error);
    if (!result.valid()) {
      sol::error err = result;
      return std::string(err.what());
    }
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

// ---------------------------------------------------------------------------
// LuaEditorDialog
// ---------------------------------------------------------------------------

class LuaEditorDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"name":"Reactive Script Editor","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kLuaEditorDialogUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // -- Timeseries list (left panel) --
    wd.setListItems("series_list", series_names_);

    // -- Code editors --
    wd.setCodeContent("global_editor", global_code_)
        .setCodeLanguage("global_editor", "lua")
        .setCodeContent("code_editor", code_)
        .setCodeLanguage("code_editor", "lua")
        .setText("function_name", function_name_)
        .setEnabled("save_button", !function_name_.empty() && !code_.empty())
        .setEnabled("run_button", !function_name_.empty() && !code_.empty() && !terminal_visible_);

    // Terminal: show/hide based on validation state
    wd.setVisible("terminal_output", terminal_visible_);
    if (terminal_visible_) {
      wd.setPlainText("terminal_output", terminal_text_);
    }

    // -- Library tab --
    std::vector<std::string> visible_names;
    for (const auto& [name, snippet] : saved_snippets_) {
      if (library_search_.empty() || name.find(library_search_) != std::string::npos) {
        visible_names.push_back(name);
      }
    }
    wd.setListItems("library_list", visible_names);
    wd.setEnabled("library_use", !library_selected_.empty());
    wd.setEnabled("library_delete", !library_selected_.empty());

    // Library preview
    if (!library_selected_.empty()) {
      auto it = saved_snippets_.find(library_selected_);
      if (it != saved_snippets_.end()) {
        std::string preview;
        if (!it->second.global_code.empty()) {
          preview += it->second.global_code + "\n\n";
        }
        preview += "function " + it->second.function_name + "(tracker_time)\n";
        preview += it->second.code;
        preview += "\nend";
        wd.setPlainText("library_preview", preview);
      }
    } else {
      wd.setPlainText("library_preview", "");
    }

    // Tab control (switch to Editor when loading snippet from Library)
    if (switch_to_tab_ >= 0) {
      wd.setTabIndex("main_tabs", switch_to_tab_);
      switch_to_tab_ = -1;
    }

    // Run triggers dialog accept (executes the script)
    if (run_requested_) {
      run_requested_ = false;
      wd.requestAccept();
    }

    return wd.toJson();
  }

  bool onCodeChanged(std::string_view name, std::string_view code) override {
    if (name == "code_editor") {
      code_ = std::string(code);
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      return true;
    }
    if (name == "global_editor") {
      global_code_ = std::string(code);
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "function_name") {
      function_name_ = std::string(text);
      return true;
    }
    if (name == "library_search") {
      library_search_ = std::string(text);
      library_selected_.clear();
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "save_button" && !function_name_.empty() && !code_.empty()) {
      // Save current code to the library (does NOT execute)
      saved_snippets_[function_name_] = SnippetData{code_, global_code_, function_name_};
      return true;
    }
    if (name == "run_button" && !function_name_.empty() && !code_.empty()) {
      run_requested_ = true;
      return true;
    }
    if (name == "library_use") {
      return loadSelectedSnippet();
    }
    if (name == "library_delete" && !library_selected_.empty()) {
      saved_snippets_.erase(library_selected_);
      library_selected_.clear();
      return true;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "library_list") {
      library_selected_ = items.empty() ? "" : items.front();
      return true;
    }
    return false;
  }

  bool onItemDoubleClicked(std::string_view name, int /*index*/) override {
    if (name == "library_list") {
      return loadSelectedSnippet();
    }
    return false;
  }

  bool onTick() override {
    if (!validation_pending_) return false;

    ++validation_tick_counter_;
    if (validation_tick_counter_ < kValidationDebounce) return false;

    validation_pending_ = false;
    validation_tick_counter_ = 0;

    std::string err = validateLuaSyntax(global_code_, code_);
    if (err.empty()) {
      terminal_visible_ = false;
      terminal_text_.clear();
    } else {
      terminal_visible_ = true;
      terminal_text_ = err;
    }
    return true;
  }

  std::string saveConfig() const override {
    nlohmann::json j;
    j["current"]["code"] = code_;
    j["current"]["global_code"] = global_code_;
    j["current"]["function_name"] = function_name_;

    nlohmann::json lib = nlohmann::json::object();
    for (const auto& [name, snippet] : saved_snippets_) {
      lib[name] = {
          {"code", snippet.code},
          {"global_code", snippet.global_code},
          {"function_name", snippet.function_name},
      };
    }
    j["library"] = lib;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) return false;

    // New schema (nested "current" object)
    if (j.contains("current") && j["current"].is_object()) {
      auto& cur = j["current"];
      code_ = cur.value("code", std::string{});
      global_code_ = cur.value("global_code", std::string{});
      function_name_ = cur.value("function_name", std::string{});
    } else {
      // Backward compat: old flat schema
      code_ = j.value("code", std::string{});
      function_name_ = j.value("function_name", std::string{});
      global_code_.clear();
    }

    // Load library
    saved_snippets_.clear();
    if (j.contains("library") && j["library"].is_object()) {
      for (auto& [key, val] : j["library"].items()) {
        saved_snippets_[key] = SnippetData{
            val.value("code", ""),
            val.value("global_code", ""),
            val.value("function_name", key),
        };
      }
    }

    terminal_visible_ = false;
    terminal_text_.clear();
    validation_pending_ = false;
    library_selected_.clear();
    library_search_.clear();
    switch_to_tab_ = -1;
    return true;
  }

  [[nodiscard]] const std::string& code() const { return code_; }
  [[nodiscard]] const std::string& globalCode() const { return global_code_; }
  [[nodiscard]] const std::string& functionName() const { return function_name_; }

  void setSeriesNames(std::vector<std::string> names) { series_names_ = std::move(names); }

 private:
  bool loadSelectedSnippet() {
    if (library_selected_.empty()) return false;
    auto it = saved_snippets_.find(library_selected_);
    if (it == saved_snippets_.end()) return false;

    code_ = it->second.code;
    global_code_ = it->second.global_code;
    function_name_ = it->second.function_name;
    switch_to_tab_ = 0;  // Switch back to Editor tab
    validation_pending_ = true;
    validation_tick_counter_ = 0;
    return true;
  }

  std::string code_ = "-- Write your Lua function body here.\n"
                       "-- It receives tracker_time as parameter.\n"
                       "-- Example:\n"
                       "--   local series = TimeseriesView(\"my_field\")\n"
                       "--   local val = series:atTime(tracker_time)\n";
  std::string global_code_;
  std::string function_name_;
  bool run_requested_ = false;

  // Terminal / validation
  std::string terminal_text_;
  bool terminal_visible_ = false;
  bool validation_pending_ = false;
  int validation_tick_counter_ = 0;
  static constexpr int kValidationDebounce = 5;  // 5 * 50ms = 250ms

  // Timeseries (populated by toolbox before dialog opens)
  std::vector<std::string> series_names_;

  // Library
  std::map<std::string, SnippetData> saved_snippets_;
  std::string library_search_;
  std::string library_selected_;
  int switch_to_tab_ = -1;  // -1 = no programmatic switch
};

// ---------------------------------------------------------------------------
// SeriesAccessor — read-only access to a series from the catalog
// ---------------------------------------------------------------------------

struct SeriesAccessor {
  std::vector<double> timestamps;
  std::vector<double> values;

  [[nodiscard]] size_t size() const { return timestamps.size(); }

  sol::object at(size_t index, sol::this_state s) const {
    if (index >= timestamps.size()) return sol::make_object(s, sol::nil);
    sol::state_view lua(s);
    sol::table result = lua.create_table();
    result[1] = timestamps[index];
    result[2] = values[index];
    return result;
  }

  double atTime(double t) const {
    if (timestamps.empty()) return 0.0;
    // Binary search for the closest timestamp.
    auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
    if (it == timestamps.end()) return values.back();
    if (it == timestamps.begin()) return values.front();
    size_t idx = static_cast<size_t>(it - timestamps.begin());
    // Linear interpolation between idx-1 and idx.
    double t0 = timestamps[idx - 1];
    double t1 = timestamps[idx];
    double v0 = values[idx - 1];
    double v1 = values[idx];
    if (t1 == t0) return v0;
    double alpha = (t - t0) / (t1 - t0);
    return v0 + alpha * (v1 - v0);
  }
};

// ---------------------------------------------------------------------------
// CreatedSeries — writable output series
// ---------------------------------------------------------------------------

struct CreatedSeries {
  std::string name;
  std::vector<double> timestamps;
  std::vector<double> values;

  void push_back(double t, double v) {
    timestamps.push_back(t);
    values.push_back(v);
  }

  void clear() {
    timestamps.clear();
    values.clear();
  }

  [[nodiscard]] size_t size() const { return timestamps.size(); }
};

// ---------------------------------------------------------------------------
// LuaEditorToolbox
// ---------------------------------------------------------------------------

class LuaEditorToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  void* dialogContext() override {
    if (toolboxHostBound()) {
      auto host = toolboxHost();
      auto catalog = host.catalogSnapshot();
      if (catalog) {
        series_map_.clear();
        series_names_.clear();

        auto all_fields = catalog->fields();
        for (const auto& topic : catalog->topics()) {
          std::string topic_name(PJ::sdk::toStringView(topic.name));
          for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
            const auto& f = all_fields[fi];
            std::string name = topic_name + "/" + std::string(PJ::sdk::toStringView(f.name));
            series_names_.push_back(name);

            auto series = host.readSeries(f.handle);
            if (series) {
              auto ts = series->timestamps();
              const auto& raw = series->raw();
              size_t count = ts.size();

              SeriesAccessor sa;
              sa.timestamps.resize(count);
              sa.values.resize(count);
              for (size_t i = 0; i < count; ++i) {
                sa.timestamps[i] = static_cast<double>(ts[i]);
                sa.values[i] = raw.values.as_float64[i];
              }
              series_map_[name] = std::move(sa);
            }
          }
        }
      }
      dialog_.setSeriesNames(series_names_);
    }
    return &dialog_;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("invalid config JSON");
    }
    if (!dialog_.code().empty() && !dialog_.functionName().empty() && toolboxHostBound() &&
        runtimeHostBound()) {
      return executeLuaScript();
    }
    return PJ::okStatus();
  }

 private:
  PJ::Status executeLuaScript() {
    const std::string& code = dialog_.code();
    const std::string& global_code = dialog_.globalCode();
    const std::string& func_name = dialog_.functionName();

    if (code.empty() || func_name.empty()) {
      return PJ::unexpected("empty code or function name");
    }

    // Set up Lua state.
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

    // Register TimeseriesView as a callable that returns a SeriesAccessor.
    lua["TimeseriesView"] = [this, &lua](const std::string& name) -> sol::object {
      auto it = series_map_.find(name);
      if (it == series_map_.end()) return sol::make_object(lua, sol::nil);
      return sol::make_object(lua, it->second);
    };

    // Register Timeseries constructor.
    std::unordered_map<std::string, CreatedSeries> created;
    lua["Timeseries"] = [&created](const std::string& name) -> CreatedSeries& {
      return created[name];
    };

    // Register GetSeriesNames.
    lua["GetSeriesNames"] = [this]() -> std::vector<std::string> {
      return series_names_;
    };

    // Register SeriesAccessor type.
    std::string sa_name = "_SeriesAccessor";
    auto sa_type = lua.new_usertype<SeriesAccessor>(sa_name);
    sa_type["size"] = &SeriesAccessor::size;
    sa_type["at"] = &SeriesAccessor::at;
    sa_type["atTime"] = &SeriesAccessor::atTime;

    // Register CreatedSeries type.
    std::string cs_name = "_CreatedSeries";
    auto cs_type = lua.new_usertype<CreatedSeries>(cs_name);
    cs_type["push_back"] = &CreatedSeries::push_back;
    cs_type["clear"] = &CreatedSeries::clear;
    cs_type["size"] = &CreatedSeries::size;

    // Execute global code first (variables, helpers, imports).
    if (!global_code.empty()) {
      auto global_result = lua.safe_script(global_code, sol::script_pass_on_error);
      if (!global_result.valid()) {
        sol::error err = global_result;
        return PJ::unexpected("Lua global code error: " + std::string(err.what()));
      }
    }

    // Wrap user code in a function and execute.
    std::string wrapped = "function _pj_user_func(tracker_time)\n" + code + "\nend";
    auto parse_result = lua.safe_script(wrapped, sol::script_pass_on_error);
    if (!parse_result.valid()) {
      sol::error err = parse_result;
      return PJ::unexpected("Lua parse error: " + std::string(err.what()));
    }

    // Execute once with tracker_time = 0 (batch mode).
    auto exec_result = lua["_pj_user_func"](0.0);
    if (!exec_result.valid()) {
      sol::error err = exec_result;
      return PJ::unexpected("Lua runtime error: " + std::string(err.what()));
    }

    // Write created series to the datastore.
    if (!created.empty() && toolboxHostBound()) {
      auto host = toolboxHost();
      auto source = host.createDataSource("lua_script");
      if (!source) {
        return PJ::unexpected("failed to create data source");
      }

      for (auto& [name, series] : created) {
        auto topic = host.ensureTopic(*source, func_name + "/" + name);
        if (!topic) continue;

        for (size_t i = 0; i < series.size(); ++i) {
          auto ts = static_cast<int64_t>(series.timestamps[i]);
          const PJ::sdk::NamedFieldValue fields[] = {
              {.name = "value", .value = series.values[i]},
          };
          auto status = host.appendRecord(*topic, ts, PJ::Span(fields));
          if (!status) {
            return PJ::unexpected("failed to append record: " + std::string(status.error()));
          }
        }
      }

      runtimeHost().notifyDataChanged();
    }

    size_t total_points = 0;
    for (const auto& [_, s] : created) {
      total_points += s.size();
    }
    runtimeHost().reportMessage(PJ::ToolboxMessageLevel::kInfo,
                                "Executed '" + func_name + "': " + std::to_string(created.size()) +
                                    " series, " + std::to_string(total_points) + " points");
    return PJ::okStatus();
  }

  LuaEditorDialog dialog_;
  std::unordered_map<std::string, SeriesAccessor> series_map_;
  std::vector<std::string> series_names_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(LuaEditorToolbox, kLuaEditorManifest)
PJ_DIALOG_PLUGIN(LuaEditorDialog)
