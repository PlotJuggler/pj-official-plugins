#include <nlohmann/json.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sol/sol.hpp>

#ifdef PJ_REACTIVE_HAS_PYTHON
#include <pybind11/embed.h>
#include <pybind11/stl.h>
namespace py = pybind11;
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "reactive_script_editor_dialog_ui.hpp"
#include "reactive_script_editor_manifest.hpp"

namespace {

// ---------------------------------------------------------------------------
// SnippetData — stored function in the library
// ---------------------------------------------------------------------------

struct SnippetData {
  std::string code;
  std::string global_code;
  std::string function_name;
};

std::filesystem::path luaEditorRecentPath() {
  return PJ::sdk::userDataDir() / "toolbox_reactive_scripts_editor" / "recent.json";
}

void persistRecentToDisk(const std::filesystem::path& path, const std::vector<SnippetData>& recent) {
  try {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::json j = nlohmann::json::array();
    for (const auto& s : recent) {
      j.push_back({{"code", s.code}, {"global_code", s.global_code}, {"function_name", s.function_name}});
    }
    std::ofstream out(path);
    if (out) {
      out << j.dump(2);
    }
  } catch (...) {}
}

std::vector<SnippetData> loadRecentFromDisk(const std::filesystem::path& path) {
  std::vector<SnippetData> result;
  std::ifstream in(path);
  if (!in) {
    return result;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  auto j = nlohmann::json::parse(buf.str(), nullptr, false);
  if (!j.is_array()) {
    return result;
  }
  for (auto& item : j) {
    if (!item.is_object()) {
      continue;
    }
    result.push_back(
        SnippetData{
            item.value("code", std::string{}),
            item.value("global_code", std::string{}),
            item.value("function_name", std::string{}),
        });
  }
  return result;
}

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
// Python integration
//
// All embedded-CPython code (interpreter setup, syntax check, execution path,
// pybind11 type registration) is gated behind PJ_REACTIVE_HAS_PYTHON. Builds
// without that flag link no libpython, ship no Python stdlib, and surface the
// Python tab in the dialog as disabled. See CMakeLists.txt — the option is
// PJ_REACTIVE_ENABLE_PYTHON, default OFF.
// ---------------------------------------------------------------------------

constexpr bool kReactivePythonEnabled =
#ifdef PJ_REACTIVE_HAS_PYTHON
    true;
#else
    false;
#endif

#ifdef PJ_REACTIVE_HAS_PYTHON

// CPython can only be initialized once per process, so we use a static guard
// that lives until program exit. Before constructing the guard, PyConfig is
// populated so CPython searches the bundled stdlib shipped alongside the
// plugin. If the stdlib directory is absent (developer build pointing at a
// system Python), module_search_paths is left untouched and CPython uses its
// default search.
void ensurePythonInterpreter() {
  static py::scoped_interpreter guard = [] {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    auto plugin_dir = PJ::sdk::getSharedLibDir(reinterpret_cast<const void*>(&ensurePythonInterpreter));
    if (!plugin_dir.empty()) {
      auto py_root = plugin_dir / "python3.12";
      if (std::filesystem::exists(py_root)) {
        config.module_search_paths_set = 1;
        auto append = [&](const std::filesystem::path& p) {
          PyStatus status = PyWideStringList_Append(&config.module_search_paths, p.wstring().c_str());
          if (PyStatus_Exception(status)) {
            PyConfig_Clear(&config);
            Py_ExitStatusException(status);
          }
        };
        append(py_root / "Lib");
#if defined(_WIN32)
        // On Windows native extensions (.pyd) sit in a sibling DLLs/ dir.
        append(py_root / "DLLs");
        // Windows won't automatically search the plugin dir or DLLs/ when CPython
        // loads .pyd modules, so register them explicitly. python3XX.dll lives
        // next to the plugin binary (see CMake POST_BUILD copy); aux DLLs that
        // the .pyd modules link against live under DLLs/.
        AddDllDirectory(plugin_dir.wstring().c_str());
        AddDllDirectory((py_root / "DLLs").wstring().c_str());
#endif
      }
    }
    return py::scoped_interpreter(&config);  // calls Py_InitializeFromConfig and PyConfig_Clear
  }();
  (void)guard;
}

// indentPythonCode — wrap user code in a def block with 4-space indentation.
std::string indentPythonCode(const std::string& code) {
  std::string result = "def _pj_user_func(tracker_time):\n";
  std::istringstream stream(code);
  std::string line;
  while (std::getline(stream, line)) {
    result += "    " + line + "\n";
  }
  if (code.empty()) {
    result += "    pass\n";
  }
  return result;
}

// validatePythonSyntax — lightweight parse check (no execution).
std::string validatePythonSyntax(const std::string& global_code, const std::string& function_code) {
  ensurePythonInterpreter();
  try {
    if (!global_code.empty()) {
      py::exec("compile(" + py::repr(py::str(global_code)).cast<std::string>() + ", '<global>', 'exec')");
    }
    std::string wrapped = indentPythonCode(function_code);
    py::exec("compile(" + py::repr(py::str(wrapped)).cast<std::string>() + ", '<editor>', 'exec')");
  } catch (const py::error_already_set& e) {
    return e.what();
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

#endif  // PJ_REACTIVE_HAS_PYTHON

// ---------------------------------------------------------------------------
// ReactiveScriptEditorDialog
// ---------------------------------------------------------------------------

class ReactiveScriptEditorDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kReactiveScriptEditorManifest;
  }

  std::string ui_content() const override {
    return kReactiveScriptEditorDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // -- Active Scripts: in-memory only, empty on restart (matches PJ3)
    std::vector<std::string> active_names;
    for (const auto& s : active_scripts_) {
      active_names.push_back(s.function_name);
    }
    wd.setListItems("series_list", active_names);

    // -- Recent scripts (independent history with full data copies, matches PJ3 listWidgetRecent)
    std::vector<std::string> recent_names;
    for (const auto& s : recent_snippets_) {
      recent_names.push_back(s.function_name);
    }
    wd.setListItems("recent_list", recent_names);

    // -- Language selector --
    bool is_lua = (language_ == "lua");
    wd.setChecked("radio_lua", is_lua);
    wd.setChecked("radio_python", !is_lua);
    // When Python is not compiled in, force-disable the radio so users can't
    // pick a backend that won't run. Lua remains the only option.
    if (!kReactivePythonEnabled) {
      wd.setEnabled("radio_python", false);
    }

    // -- Code editors --
    wd.setCodeContent("global_editor", global_code_)
        .setCodeLanguage("global_editor", language_)
        .setCodeContent("code_editor", code_)
        .setCodeLanguage("code_editor", language_)
        .setText("function_name", function_name_)
        .setEnabled("save_button", !function_name_.empty() && !code_.empty())
        .setEnabled("run_button", !function_name_.empty() && !code_.empty() && !has_syntax_error_)
        .setEnabled("deleteButton", !active_selected_.empty());

    // -- Dynamic labels based on language --
    if (is_lua) {
      wd.setText("funcLabel", "function(tracker_time)");
      wd.setText("endLabel", "end");
      wd.setVisible("endLabel", true);
    } else {
      wd.setText("funcLabel", "def transform(tracker_time):");
      wd.setVisible("endLabel", false);
    }

    // Terminal: show/hide based on validation state
    wd.setVisible("terminal_output", terminal_visible_);
    if (terminal_visible_) {
      wd.setPlainText("terminal_output", terminal_text_);
    }

    // -- Library tab (matches PJ3 textLibrary) --
    wd.setCodeContent("library_editor", library_code_).setCodeLanguage("library_editor", "lua");
    wd.setEnabled("library_apply", !library_valid_);

    // Semaphore: green = library OK, red = syntax error
    if (library_valid_) {
      wd.setText("labelSemaphore", "<html><body><span style='color:#00aa00; font-size:22px;'>⬤</span></body></html>");
    } else {
      wd.setText("labelSemaphore", "<html><body><span style='color:#cc0000; font-size:22px;'>⬤</span></body></html>");
    }

    // Tab control (switch to Editor when loading snippet from Library)
    if (switch_to_tab_ >= 0) {
      wd.setTabIndex("main_tabs", switch_to_tab_);
      switch_to_tab_ = -1;
    }

    return wd.toJson();
  }

  bool onCodeChanged(std::string_view name, std::string_view code) override {
    if (name == "code_editor") {
      code_ = std::string(code);
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      terminal_sticky_ = false;
      return true;
    }
    if (name == "global_editor") {
      global_code_ = std::string(code);
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      terminal_sticky_ = false;
      return true;
    }
    if (name == "library_editor") {
      library_code_ = std::string(code);
      // Validate library Lua syntax — pass as global code, empty function body
      library_valid_ = validateLuaSyntax(library_code_, "").empty();
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "function_name") {
      function_name_ = std::string(text);
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "radio_lua" && checked) {
      language_ = "lua";
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      return true;
    }
    if (name == "radio_python" && checked) {
      // Reject the toggle when Python is not compiled in — the radio is
      // already disabled in widget_data(), but a stale checked event from a
      // restored config could still arrive here.
      if (!kReactivePythonEnabled) {
        return false;
      }
      language_ = "python";
      validation_pending_ = true;
      validation_tick_counter_ = 0;
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "save_button" && !function_name_.empty() && !code_.empty()) {
      SnippetData snippet{code_, global_code_, function_name_};
      // Add to Active Scripts (in-memory, matches PJ3 listWidgetFunctions)
      active_scripts_.erase(
          std::remove_if(
              active_scripts_.begin(), active_scripts_.end(),
              [&](const SnippetData& s) { return s.function_name == function_name_; }),
          active_scripts_.end());
      active_scripts_.push_back(snippet);
      // Add to Recent scripts (persisted, matches PJ3 listWidgetRecent)
      addToRecent(snippet);
      return true;
    }
    if (name == "run_button" && !function_name_.empty() && !code_.empty()) {
      std::string msg;
      try {
        msg = run_callback_ ? run_callback_(code_, global_code_, function_name_)
                            : std::string("Run unavailable: toolbox not bound");
      } catch (const std::exception& e) {
        msg = std::string("Error: unhandled exception: ") + e.what();
      } catch (...) {
        msg = "Error: unhandled exception (non-std)";
      }
      terminal_text_ = msg;
      terminal_visible_ = true;
      terminal_sticky_ = true;
      return true;
    }
    // Delete button in Active Scripts (in-memory only, matches PJ3 pushButtonDelete)
    if (name == "deleteButton" && !active_selected_.empty()) {
      active_scripts_.erase(
          std::remove_if(
              active_scripts_.begin(), active_scripts_.end(),
              [&](const SnippetData& s) { return s.function_name == active_selected_; }),
          active_scripts_.end());
      active_selected_.clear();
      return true;
    }
    // Restore default library code (matches PJ3 pushButtonDefaultLibrary)
    if (name == "library_restore") {
      library_code_ = kDefaultLibraryCode;
      library_valid_ = true;
      return true;
    }
    // Apply changes — mark library as applied (matches PJ3 pushButtonApplyLibrary)
    if (name == "library_apply") {
      library_valid_ = true;
      return true;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    // Selecting in either Active Scripts or Recent scripts selects the same snippet
    if (name == "series_list" || name == "recent_list") {
      active_selected_ = items.empty() ? "" : items.front();
      return true;
    }
    return false;
  }

  bool onItemDoubleClicked(std::string_view name, int index) override {
    // Double-click on Active Scripts (in-memory) or Recent scripts (PJ3 restoreRecent)
    // loads that snippet into the editor.
    if (name == "series_list" && index >= 0 && index < static_cast<int>(active_scripts_.size())) {
      applySnippet(active_scripts_[static_cast<size_t>(index)]);
      return true;
    }
    if (name == "recent_list" && index >= 0 && index < static_cast<int>(recent_snippets_.size())) {
      applySnippet(recent_snippets_[static_cast<size_t>(index)]);
      return true;
    }
    return false;
  }

  bool onTick() override {
    if (!validation_pending_) {
      return false;
    }

    ++validation_tick_counter_;
    if (validation_tick_counter_ < kValidationDebounce) {
      return false;
    }

    validation_pending_ = false;
    validation_tick_counter_ = 0;

    std::string err;
    if (language_ == "lua") {
      err = validateLuaSyntax(global_code_, code_);
    } else {
#ifdef PJ_REACTIVE_HAS_PYTHON
      err = validatePythonSyntax(global_code_, code_);
#else
      err = "Python is not available in this build (Lua-only).";
#endif
    }
    if (err.empty()) {
      has_syntax_error_ = false;
      // Preserve Run output if sticky; otherwise clear terminal.
      if (!terminal_sticky_) {
        terminal_visible_ = false;
        terminal_text_.clear();
      }
    } else {
      has_syntax_error_ = true;
      terminal_sticky_ = false;  // A new error overrides any prior Run output.
      terminal_visible_ = true;
      terminal_text_ = err;
    }
    return true;
  }

  // Workspace session config carries only the "current" snippet. The library
  // lives on disk (per-user) and is owned by the toolbox.
  std::string saveConfig() const override {
    nlohmann::json j;
    j["current"]["code"] = code_;
    j["current"]["global_code"] = global_code_;
    j["current"]["function_name"] = function_name_;
    j["current"]["language"] = language_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) {
      return false;
    }

    // New schema (nested "current" object)
    if (j.contains("current") && j["current"].is_object()) {
      auto& cur = j["current"];
      code_ = cur.value("code", std::string{});
      global_code_ = cur.value("global_code", std::string{});
      function_name_ = cur.value("function_name", std::string{});
      language_ = cur.value("language", std::string{"lua"});
    } else {
      // Backward compat: old flat schema
      code_ = j.value("code", std::string{});
      function_name_ = j.value("function_name", std::string{});
      global_code_.clear();
    }

    terminal_visible_ = false;
    terminal_text_.clear();
    validation_pending_ = false;
    switch_to_tab_ = -1;
    return true;
  }

  [[nodiscard]] const std::string& code() const {
    return code_;
  }
  [[nodiscard]] const std::string& globalCode() const {
    return global_code_;
  }
  [[nodiscard]] const std::string& functionName() const {
    return function_name_;
  }
  [[nodiscard]] const std::string& language() const {
    return language_;
  }

  void setSeriesNames(std::vector<std::string> names) {
    series_names_ = std::move(names);
  }

  using RunCallback =
      std::function<std::string(const std::string& code, const std::string& global, const std::string& name)>;
  void setRunCallback(RunCallback cb) {
    run_callback_ = std::move(cb);
  }

  void setRecentSnippets(std::vector<SnippetData> recent) {
    recent_snippets_ = std::move(recent);
  }

  void addToRecent(const SnippetData& snippet) {
    recent_snippets_.erase(
        std::remove_if(
            recent_snippets_.begin(), recent_snippets_.end(),
            [&](const SnippetData& s) { return s.function_name == snippet.function_name; }),
        recent_snippets_.end());
    recent_snippets_.insert(recent_snippets_.begin(), snippet);
    if (recent_snippets_.size() > 10) {
      recent_snippets_.resize(10);
    }
    persistRecentToDisk(luaEditorRecentPath(), recent_snippets_);
  }

 private:
  // Load a snippet's code/global/name into the editor and switch to the Editor tab.
  void applySnippet(const SnippetData& s) {
    code_ = s.code;
    global_code_ = s.global_code;
    function_name_ = s.function_name;
    switch_to_tab_ = 0;
    validation_pending_ = true;
    validation_tick_counter_ = 0;
  }

  std::string code_;
  std::string global_code_;
  std::string function_name_;
  std::string language_ = "lua";

  // Library code (matches PJ3 textLibrary) — editable block of Lua helper functions
  static constexpr const char* kDefaultLibraryCode =
      "-- Helper functions usable in your reactive scripts\n"
      "\n"
      "function CreateSeriesFromArray(new_series, prefix, suffix_X, suffix_Y, timestamp)\n"
      "  new_series:clear()\n"
      "  local index = 0\n"
      "  while true do\n"
      "    local x = index\n"
      "    if suffix_X ~= nil then\n"
      "      local series_x = TimeseriesView.find(string.format(\"%s.%d/%s\", prefix, index, suffix_X))\n"
      "      if series_x == nil then break end\n"
      "      x = series_x:atTime(timestamp)\n"
      "    end\n"
      "    local series_y = TimeseriesView.find(string.format(\"%s.%d/%s\", prefix, index, suffix_Y))\n"
      "    if series_y == nil then break end\n"
      "    local y = series_y:atTime(timestamp)\n"
      "    new_series:push_back(x, y)\n"
      "    index = index + 1\n"
      "  end\n"
      "end\n"
      "\n"
      "function GetSeriesNamesByPrefix(prefix)\n"
      "  local all_names = GetSeriesNames()\n"
      "  local filtered = {}\n"
      "  for i, name in ipairs(all_names) do\n"
      "    if name:find(prefix, 1, #prefix) then\n"
      "      table.insert(filtered, name)\n"
      "    end\n"
      "  end\n"
      "  return filtered\n"
      "end\n"
      "\n"
      "function ApplyOffsetInPlace(series, delta_x, delta_y)\n"
      "  for index=0, series:size()-1 do\n"
      "    local x, y = series:at(index)\n"
      "    series:set(index, x + delta_x, y + delta_y)\n"
      "  end\n"
      "end\n";

  std::string library_code_ = kDefaultLibraryCode;
  bool library_valid_ = true;  // true when library_code_ compiles without errors

  // Terminal / validation
  std::string terminal_text_;
  bool terminal_visible_ = false;
  bool terminal_sticky_ = false;   // True while terminal shows Run output; cleared on code edit or new syntax error.
  bool has_syntax_error_ = false;  // Tracks last validation result; gates the Run button.
  bool validation_pending_ = false;
  int validation_tick_counter_ = 0;
  static constexpr int kValidationDebounce = 5;  // 5 * 50ms = 250ms

  // Run execution (injected by the toolbox in dialogContext).
  RunCallback run_callback_;

  // Timeseries (populated by toolbox before dialog opens)
  std::vector<std::string> series_names_;

  std::string active_selected_;  // selected in series_list or recent_list
  // Active Scripts: in-memory only, empty on restart (matches PJ3 listWidgetFunctions)
  std::vector<SnippetData> active_scripts_;
  // Recent history: persisted, survives restart (matches PJ3 listWidgetRecent)
  std::vector<SnippetData> recent_snippets_;
  int switch_to_tab_ = -1;  // -1 = no programmatic switch
};

// ---------------------------------------------------------------------------
// SeriesAccessor — read-only access to a series from the catalog
// ---------------------------------------------------------------------------

struct TimePoint {
  double t;
  double v;
};

struct SeriesAccessor {
  std::vector<double> timestamps;
  std::vector<double> values;

  [[nodiscard]] size_t size() const {
    return timestamps.size();
  }

  [[nodiscard]] std::optional<TimePoint> at(size_t index) const {
    if (index >= timestamps.size()) {
      return std::nullopt;
    }
    return TimePoint{timestamps[index], values[index]};
  }

  [[nodiscard]] double atTime(double t) const {
    if (timestamps.empty()) {
      return 0.0;
    }
    // Binary search for the closest timestamp.
    auto it = std::lower_bound(timestamps.begin(), timestamps.end(), t);
    if (it == timestamps.end()) {
      return values.back();
    }
    if (it == timestamps.begin()) {
      return values.front();
    }
    size_t idx = static_cast<size_t>(it - timestamps.begin());
    // Linear interpolation between idx-1 and idx.
    double t0 = timestamps[idx - 1];
    double t1 = timestamps[idx];
    double v0 = values[idx - 1];
    double v1 = values[idx];
    if (t1 == t0) {
      return v0;
    }
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

  [[nodiscard]] size_t size() const {
    return timestamps.size();
  }
};

#ifdef PJ_REACTIVE_HAS_PYTHON
// Register SeriesAccessor and CreatedSeries with pybind11 inside the embedded
// interpreter. Called lazily from executePythonScriptImpl() after the
// scoped_interpreter is live; registering at static-init time via
// PYBIND11_EMBEDDED_MODULE would fault when the plugin is dlopen'ed from a
// process that already has Python initialized (for example the release
// verification tool).
void registerPjTypes() {
  static const bool once = [] {
    py::module_ m = py::module_::import("__main__");
    py::class_<SeriesAccessor>(m, "_PJSeriesAccessor")
        .def("size", &SeriesAccessor::size)
        .def(
            "at",
            [](const SeriesAccessor& sa, size_t index) -> py::object {
              auto pt = sa.at(index);
              if (!pt) {
                return py::none();
              }
              return py::make_tuple(pt->t, pt->v);
            })
        .def("atTime", &SeriesAccessor::atTime);

    py::class_<CreatedSeries>(m, "_PJCreatedSeries")
        .def("push_back", &CreatedSeries::push_back)
        .def("clear", &CreatedSeries::clear)
        .def("size", &CreatedSeries::size);
    return true;
  }();
  (void)once;
}
#endif  // PJ_REACTIVE_HAS_PYTHON

// ---------------------------------------------------------------------------
// ReactiveScriptEditorToolbox
// ---------------------------------------------------------------------------

class ReactiveScriptEditorToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    auto_run_pending_ = false;
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
            if (series && series->type() == PJ::PrimitiveType::kFloat64) {
              auto ts = series->timestamps();
              const double* values = series->valuesAsFloat64();
              size_t count = ts.size();

              if (values != nullptr) {
                SeriesAccessor sa;
                sa.timestamps.resize(count);
                sa.values.resize(count);
                for (size_t i = 0; i < count; ++i) {
                  sa.timestamps[i] = static_cast<double>(ts[i]);
                  sa.values[i] = values[i];
                }
                series_map_[name] = std::move(sa);
              }
            }
          }
        }
      }
      dialog_.setSeriesNames(series_names_);
    }
    ensureRecentLoaded();
    dialog_.setRunCallback(
        [this](const std::string& c, const std::string& g, const std::string& n) { return executeScript(c, g, n); });
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("invalid config JSON");
    }

    if (auto_run_pending_ && !dialog_.code().empty() && !dialog_.functionName().empty() && toolboxHostBound() &&
        runtimeHostBound()) {
      auto_run_pending_ = false;
      std::string msg = executeScript(dialog_.code(), dialog_.globalCode(), dialog_.functionName());
      if (msg.rfind("Error:", 0) == 0) {
        return PJ::unexpected(msg);
      }
    }
    return PJ::okStatus();
  }

 private:
  // Writes script-created series to the datastore. Returns a user-facing summary
  // or an error prefixed with "Error:". Shared by all language backends.
  std::string writeCreatedSeries(
      const std::unordered_map<std::string, CreatedSeries>& created, const std::string& func_name) {
    if (!created.empty()) {
      auto host = toolboxHost();
      auto source = host.createDataSource("script_output");
      if (!source) {
        return "Error: failed to create data source";
      }

      for (const auto& [name, series] : created) {
        auto topic = host.ensureTopic(*source, func_name + "/" + name);
        if (!topic) {
          continue;
        }

        for (size_t i = 0; i < series.size(); ++i) {
          auto ts = static_cast<int64_t>(series.timestamps[i]);
          const PJ::sdk::NamedFieldValue fields[] = {
              {.name = "value", .value = series.values[i]},
          };
          auto status = host.appendRecord(*topic, ts, PJ::Span<const PJ::sdk::NamedFieldValue>(fields));
          if (!status) {
            return "Error: failed to append record: " + std::string(status.error());
          }
        }
      }

      runtimeHost().notifyDataChanged();
    }

    size_t total_points = 0;
    for (const auto& [_, s] : created) {
      total_points += s.size();
    }
    std::string summary = "Executed '" + func_name + "': " + std::to_string(created.size()) + " series, " +
                          std::to_string(total_points) + " points";
    runtimeHost().reportMessage(PJ::ToolboxMessageLevel::kInfo, summary);
    return summary;
  }

  // Dispatches script execution to the appropriate language backend.
  std::string executeScript(const std::string& code, const std::string& global_code, const std::string& func_name) {
    if (dialog_.language() == "python") {
#ifdef PJ_REACTIVE_HAS_PYTHON
      return executePythonScriptImpl(code, global_code, func_name);
#else
      return "Error: Python is not available in this build. "
             "Switch the language selector to Lua.";
#endif
    }
    return executeLuaScriptImpl(code, global_code, func_name);
  }

  // Executes a Lua script in batch mode (tracker_time = 0). Returns a user-facing
  // message: success summary or error prefixed with "Error:".
  std::string executeLuaScriptImpl(
      const std::string& code, const std::string& global_code, const std::string& func_name) {
    if (code.empty() || func_name.empty()) {
      return "Error: empty code or function name";
    }
    if (!toolboxHostBound() || !runtimeHostBound()) {
      return "Error: toolbox/runtime host not bound";
    }

    // Set up Lua state.
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table);

    // Register TimeseriesView as a callable that returns a SeriesAccessor.
    lua["TimeseriesView"] = [this, &lua](const std::string& name) -> sol::object {
      auto it = series_map_.find(name);
      if (it == series_map_.end()) {
        return sol::make_object(lua, sol::lua_nil);
      }
      return sol::make_object(lua, it->second);
    };

    // Register Timeseries constructor.
    std::unordered_map<std::string, CreatedSeries> created;
    lua["Timeseries"] = [&created](const std::string& name) -> CreatedSeries& { return created[name]; };

    // Register GetSeriesNames.
    lua["GetSeriesNames"] = [this]() -> std::vector<std::string> { return series_names_; };

    // Register SeriesAccessor type.
    std::string sa_name = "_SeriesAccessor";
    auto sa_type = lua.new_usertype<SeriesAccessor>(sa_name);
    sa_type["size"] = &SeriesAccessor::size;
    sa_type["at"] = [](const SeriesAccessor& sa, size_t index, sol::this_state s) -> sol::object {
      auto pt = sa.at(index);
      if (!pt) {
        return sol::make_object(s, sol::lua_nil);
      }
      sol::state_view lv(s);
      sol::table result = lv.create_table();
      result[1] = pt->t;
      result[2] = pt->v;
      return result;
    };
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
        return "Error: Lua global code: " + std::string(err.what());
      }
    }

    // Wrap user code in a function and execute.
    std::string wrapped = "function _pj_user_func(tracker_time)\n" + code + "\nend";
    auto parse_result = lua.safe_script(wrapped, sol::script_pass_on_error);
    if (!parse_result.valid()) {
      sol::error err = parse_result;
      return "Error: Lua parse: " + std::string(err.what());
    }

    // Execute in batch mode: call once per timestamp of the first available
    // series so the script produces a properly time-aligned output series.
    // tracker_time is passed in nanoseconds (same scale as the stored timestamps)
    // so TimeseriesView(...):atTime(tracker_time) and push_back(tracker_time, v)
    // work correctly without unit conversion.
    // Fall back to a single call at t=0 when no series are loaded.
    auto fn = lua.get<sol::protected_function>("_pj_user_func");
    const std::vector<double>* timeline = nullptr;
    if (!series_map_.empty()) {
      timeline = &series_map_.begin()->second.timestamps;
    }
    auto run_at = [&](double t) -> std::string {
      sol::protected_function_result r = fn(t);
      if (!r.valid()) {
        sol::error err = r;
        return "Error: Lua runtime: " + std::string(err.what());
      }
      return {};
    };
    if (timeline && !timeline->empty()) {
      for (double t : *timeline) {
        if (auto err = run_at(t); !err.empty()) {
          return err;
        }
      }
    } else {
      if (auto err = run_at(0.0); !err.empty()) {
        return err;
      }
    }

    return writeCreatedSeries(created, func_name);
  }

#ifdef PJ_REACTIVE_HAS_PYTHON
  // Executes a Python script in batch mode (tracker_time = 0). Returns a user-facing
  // message: success summary or error prefixed with "Error:".
  std::string executePythonScriptImpl(
      const std::string& code, const std::string& global_code, const std::string& func_name) {
    if (code.empty() || func_name.empty()) {
      return "Error: empty code or function name";
    }
    if (!toolboxHostBound() || !runtimeHostBound()) {
      return "Error: toolbox/runtime host not bound";
    }

    ensurePythonInterpreter();
    registerPjTypes();

    try {
      py::dict scope;

      // Register global functions with runtime state.
      std::unordered_map<std::string, CreatedSeries> created;

      scope["TimeseriesView"] = py::cpp_function([this](const std::string& name) -> py::object {
        auto it = series_map_.find(name);
        if (it == series_map_.end()) {
          return py::none();
        }
        return py::cast(it->second, py::return_value_policy::reference);
      });

      scope["Timeseries"] = py::cpp_function(
          [&created](const std::string& name) -> CreatedSeries& { return created[name]; },
          py::return_value_policy::reference);

      scope["GetSeriesNames"] = py::cpp_function([this]() -> std::vector<std::string> { return series_names_; });

      // Import common modules into the scope.
      scope["math"] = py::module_::import("math");

      // Execute global code.
      if (!global_code.empty()) {
        py::exec(global_code, scope);
      }

      // Wrap user code in a function and execute in batch mode:
      // call once per timestamp of the first series (same approach as Lua).
      std::string wrapped = indentPythonCode(code);
      py::exec(wrapped, scope);
      const std::vector<double>* timeline = nullptr;
      if (!series_map_.empty()) {
        timeline = &series_map_.begin()->second.timestamps;
      }
      if (timeline && !timeline->empty()) {
        for (double t : *timeline) {
          scope["_pj_t"] = t;
          py::exec("_pj_user_func(_pj_t)", scope);
        }
      } else {
        py::exec("_pj_user_func(0.0)", scope);
      }

      return writeCreatedSeries(created, func_name);

    } catch (const py::error_already_set& e) {
      return "Error: Python runtime: " + std::string(e.what());
    } catch (const std::exception& e) {
      return "Error: Python: " + std::string(e.what());
    }
  }
#endif  // PJ_REACTIVE_HAS_PYTHON

  // Loads the persisted Recent-scripts history into the dialog the first time it's
  // needed. Active Scripts are in-memory only (empty on startup, like PJ3).
  void ensureRecentLoaded() {
    if (recent_loaded_) {
      return;
    }
    dialog_.setRecentSnippets(loadRecentFromDisk(luaEditorRecentPath()));
    recent_loaded_ = true;
  }

  ReactiveScriptEditorDialog dialog_;
  std::unordered_map<std::string, SeriesAccessor> series_map_;
  std::vector<std::string> series_names_;
  bool recent_loaded_ = false;
  bool auto_run_pending_ = true;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(ReactiveScriptEditorToolbox, kReactiveScriptEditorManifest)
PJ_DIALOG_PLUGIN(ReactiveScriptEditorDialog)
