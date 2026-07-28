// SPDX-License-Identifier: MPL-2.0
//
// Transform Editor toolbox plugin for PlotJuggler 4.
// Ports the PJ3 "Custom Series" / Function Editor to the PJ4 semantic UI.
// Motor: pj.data_processors.v1 — onSave hands the host a self-describing Luau class
// (N inputs -> M outputs) run live as a DerivedEngine node. Requires SDK >= 0.12.0.
// Preview uses createEphemeralTransform (SDK 0.12+) — no local Lua runtime.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "transform_editor_dialog_ui.hpp"
#include "transform_editor_manifest.hpp"
// Auxiliary dialog/panel UIs, embedded from their .ui files at build time:
//   kHelpDialogUi       — Help (requestSubDialog), same for both tabs.
//   kFunctionLibraryUi  — live Function Library sub-panel (requestSubPanel).
//   kSaveNameUi         — "save current function" name prompt (requestSubDialog).
//   kOverwriteUi        — overwrite-existing-function confirmation (requestSubDialog).
#include "function_library_ui.hpp"
#include "overwrite_function_ui.hpp"
#include "save_function_name_ui.hpp"
#include "transform_editor_help_ui.hpp"

namespace {

// ---------------------------------------------------------------------------
// Snippet
// ---------------------------------------------------------------------------

struct Snippet {
  std::string name;
  std::string global_code;
  std::string function_body;
  std::string language = "luau";  // "luau" | "python"; legacy/builtin snippets are Luau
};

std::filesystem::path snippetLibraryPath() {
  return PJ::sdk::userDataDir() / "toolbox_transform_editor" / "snippets.json";
}

// Write the snippet library as a JSON array to `path`, creating parent dirs.
// Returns false if the directory can't be created or the file can't be written
// (callers that surface Export feedback rely on this; autosave ignores it).
// Shared by the fixed-location persistence and the user-chosen Export target.
bool saveSnippetsToPath(const std::vector<Snippet>& snippets, const std::filesystem::path& path) {
  try {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::json j = nlohmann::json::array();
    for (const auto& s : snippets) {
      j.push_back(
          {{"name", s.name},
           {"global_code", s.global_code},
           {"function_body", s.function_body},
           {"language", s.language}});
    }
    std::ofstream out(path);
    if (!out) {
      return false;
    }
    out << j.dump(2);
    return out.good();
  } catch (...) {
    return false;
  }
}

void saveSnippetsToDisk(const std::vector<Snippet>& snippets) {
  saveSnippetsToPath(snippets, snippetLibraryPath());  // best-effort autosave; failures are non-fatal
}

// Default snippets ported from PJ3's default.snippets.xml
std::vector<Snippet> defaultSnippets() {
  return {
      {"backward_difference_derivative", "prevX = 0\nprevY = 0\nis_first = true",
       "if (is_first) then\n  is_first = false\n  prevX = time\n  prevY = value\nend\n\ndx = time - prevX\ndy = value "
       "- prevY\nprevX = time\nprevY = value\n\nreturn dy/dx"},
      {"central_difference_derivative",
       "firstX = 0\nfirstY = 0\nis_first = true\nsecondX = 0\nsecondY = 0\nis_second = false",
       "if (is_first) then\n  is_first = false\n  is_second = true\n  firstX = time\n  firstY = value\nend\n\nif "
       "(is_second) then\n  is_second = false\n  secondX = time\n  secondY = value\nend\n\ndx = time - firstX\ndy = "
       "value - firstY\nfirstX = secondX\nfirstY = secondY\nsecondX = time\nsecondY = value\n\nreturn dy/dx"},
      {"average_two_curves", "", "return (value+v1)/2"},
      {"integral", "prevX = 0\nintegral = 0\nis_first = true",
       "if (is_first) then\n  is_first = false\n  prevX = time\nend\n\ndx = time - prevX\nprevX = time\nintegral = "
       "integral + value*dx\n\nreturn integral"},
      {"rad_to_deg", "", "return value*180/3.14159"},
      {"remove_offset", "is_first = true\nfirst_value = 0",
       "if (is_first) then\n  is_first = false\n  first_value = value\nend\n\nreturn value - first_value"},
      {"quat_to_roll", "",
       "w = value\nx = v1\ny = v2\nz = v3\n\ndcm21 = 2 * (w * x + y * z)\ndcm22 = w*w - x*x - y*y + z*z\n\nroll = "
       "math.atan2(dcm21, dcm22)\n\nreturn roll"},
      {"quat_to_pitch", "",
       "w = value\nx = v1\ny = v2\nz = v3\n\ndcm20 = 2 * (x * z - w * y)\n\npitch = math.asin(-dcm20)\n\nreturn pitch"},
      {"quat_to_yaw", "",
       "w = value\nx = v1\ny = v2\nz = v3\n\ndcm10 = 2 * (x * y + w * z)\ndcm00 = w*w + x*x - y*y - z*z\n\nyaw = "
       "math.atan2(dcm10, dcm00)\n\nreturn yaw"},
  };
}

// Read a snippet library (JSON array) from `path`. Returns the parsed snippets
// (possibly empty, for a "[]" library), or nullopt when the file cannot be
// opened or does not hold a JSON array. The nullopt vs empty distinction lets
// callers tell "no readable library" apart from "an explicitly empty one".
// Shared by the fixed-location load and the user-chosen Import source.
std::optional<std::vector<Snippet>> loadSnippetsFromPath(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  auto j = nlohmann::json::parse(buf.str(), nullptr, false);
  if (!j.is_array()) {
    return std::nullopt;
  }
  std::vector<Snippet> result;
  for (auto& item : j) {
    if (!item.is_object()) {
      continue;
    }
    result.push_back(
        {item.value("name", std::string{}), item.value("global_code", std::string{}),
         item.value("function_body", std::string{}), item.value("language", std::string{"luau"})});
  }
  return result;
}

// The persisted library, or the built-in defaults when none can be read yet —
// a missing, unreadable, or corrupt file all fall back to the defaults (so a
// transient read failure never silently presents an empty library).
std::vector<Snippet> loadSnippetsFromDisk() {
  if (auto loaded = loadSnippetsFromPath(snippetLibraryPath())) {
    return std::move(*loaded);
  }
  return defaultSnippets();
}

// The output-name field doubles as a comma-separated list: "roll,pitch,yaw" declares
// three output topics and the body must `return r, p, q` (M values, positional).
// Whitespace around each name is trimmed; empty entries drop. One name => one output
// (the common case).
inline std::vector<std::string> splitOutputNames(const std::string& field) {
  std::vector<std::string> names;
  std::size_t start = 0;
  while (start <= field.size()) {
    const std::size_t comma = field.find(',', start);
    const std::size_t end = (comma == std::string::npos) ? field.size() : comma;
    std::string name = field.substr(start, end - start);
    const std::size_t b = name.find_first_not_of(" \t");
    const std::size_t e = name.find_last_not_of(" \t");
    if (b != std::string::npos) {
      names.push_back(name.substr(b, e - b + 1));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return names;
}

// Build a complete self-describing Luau FILTER CLASS the host can compile and run
// live as a DerivedEngine node (via createTransform). The user's global code runs
// once per instance inside a factory closure, so its locals persist across calls
// (PJ3 global-variable semantics); the body becomes the per-sample function with
// `time`/`value` (and `v1..vN` for the additional sources) in scope.
//
// `num_extra` is the count of additional sources: the body function takes
// `(time, value, v1, …, v<num_extra>)`, matching the host's MIMO calculate contract
// (calculate(self, t, v, v1..vN-1) — see pj_scripting FILTER_CLASS.md). `:calculate`
// forwards its args with `...`, so any arity works; the named params just give the
// body the v1..vN identifiers. Output count is decided host-side by the `outputs`
// passed to createTransform — `:calculate` returns the body's results unchanged
// (MULTRET), so a body that `return`s M values feeds M output topics.
// Escape a string so it is safe to embed inside a DOUBLE-QUOTED Lua or Python string
// literal. Without this, a user-controlled id/name containing a quote (or backslash /
// newline) closes the literal early and the rest is parsed as CODE — i.e. a nickname
// like `a"; import os; os.system(...) ; z="` would execute arbitrary code when the
// generated script is compiled/run. Both languages accept the same C-style escapes for
// these characters, so one routine covers both backends.
inline std::string escapeForStringLiteral(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

inline std::string buildTransformScript(
    const std::string& raw_id, const std::string& raw_name, const std::string& global_code, const std::string& body,
    std::size_t num_extra, const std::string& language = "luau") {
  // Escape id/name before they are concatenated into the generated script's string
  // literals — they are user-controlled (the output-name field) and would otherwise
  // allow code injection. See escapeForStringLiteral.
  const std::string id = escapeForStringLiteral(raw_id);
  const std::string name = escapeForStringLiteral(raw_name);
  std::string params = "time, value";
  for (std::size_t k = 0; k < num_extra; ++k) {
    params += ", v" + std::to_string(k + 1);
  }

  // Python backend: emit a module with a top-level class `T` (see pj_scripting's
  // python_engine.h). The global section runs once at module level (so `global`
  // persistent state works, PJ3-style); the body becomes the function. Python is
  // whitespace-sensitive, so every body line is indented one level.
  if (language == "python") {
    const auto indent_block = [](const std::string& code) {
      std::string out;
      std::size_t start = 0;
      while (start <= code.size()) {
        const std::size_t nl = code.find('\n', start);
        const std::string line = code.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        out += "    " + line + "\n";
        if (nl == std::string::npos) {
          break;
        }
        start = nl + 1;
      }
      return out;
    };
    std::string src = "# pj-script: python\n";
    if (!global_code.empty()) {
      src += global_code + "\n\n";
    }
    src += "def _pj_fn(" + params + "):\n";
    src += indent_block(body.empty() ? "return value" : body);
    src += "\n";
    src += "class T:\n";
    src += "    id = \"" + id + "\"\n";
    src += "    name = \"" + name + "\"\n";
    src += "    output = \"double\"\n";
    src += "    @staticmethod\n";
    src += "    def create(params):\n        return T()\n";
    src += "    def calculate(self, time, value, *args):\n        return _pj_fn(time, value, *args)\n";
    return src;
  }

  // The header declares the backend; the host's inferTransformBackend reads it.
  std::string src = "-- pj-script: " + language + "\n";
  src += "local function _pj_make()\n";
  src += global_code + "\n";
  src += "  return function(" + params + ")\n";
  src += body + "\n";
  src += "  end\n";
  src += "end\n";
  src += "local T = { id = \"" + id + "\", name = \"" + name + "\", output = \"double\" }\n";
  src += "T.__index = T\n";
  src += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
  src += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
  src += "return T\n";
  return src;
}

// ---------------------------------------------------------------------------
// TransformEditorDialog
// ---------------------------------------------------------------------------

class TransformEditorDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  std::string manifest() const override {
    return kTransformEditorManifest;
  }
  std::string ui_content() const override {
    return kTransformEditorDialogUi;
  }

  PJ::WidgetData buildWidgetData() {
    PJ::WidgetData wd;

    // One-shot after loadConfig (Modify): force the editor onto the tab the series
    // was created from. Only once, so the user can freely switch tabs afterwards.
    if (pending_tab_restore_) {
      wd.setTabIndex("tabWidget", current_tab_);
      pending_tab_restore_ = false;
    }

    // Single function tab — one table of inputs (drop target). Col 0 is the radio
    // marking which row provides `value`; col 1 is the series path; col 2 is the
    // bound variable. The non-primary rows are v1, v2, ... in row order.
    wd.setDropTarget("tableSources");
    wd.setTableHeaders("tableSources", {"", "Input timeseries", "Var"});
    std::vector<std::vector<std::string>> rows;
    rows.reserve(sources_.size());
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
      rows.push_back({"", sources_[static_cast<std::size_t>(i)], variableName(i)});
    }
    wd.setTableRows("tableSources", rows);
    wd.setListPlaceholder("tableSources", "Drag & drop timeseries here");
    wd.setListItemsDeletable("tableSources", true);
    wd.setTableRadioColumn("tableSources", 0, primaryIndex());
    wd.setText("nameLineEdit", output_name_);

    // Function signature reflects the inputs: `value` (the radio-selected row) plus
    // v1..vN for the remaining series, so the user sees the identifier to reference
    // for each one in the body.
    const std::size_t num_extra = orderedExtras().size();
    std::string signature = "function( time, value";
    for (std::size_t i = 0; i < num_extra; ++i) {
      signature += ", v" + std::to_string(i + 1);
    }
    signature += " )";
    wd.setText("functionTitle", signature);

    const char* single_lang = (language_ == "python") ? "python" : "lua";
    wd.setCodeContent("globalVarsText", global_code_).setCodeLanguage("globalVarsText", single_lang);
    wd.setCodeContent("functionText", function_body_).setCodeLanguage("functionText", single_lang);
    // Reflect the active language on the radios (so loading a library snippet flips
    // them, not just the user clicking). Pushed every tick; matches language_.
    wd.setChecked("luaButton", language_ != "python");
    wd.setChecked("pythonButton", language_ == "python");

    // Function Library sub-panel (cloned from PJ3's buttonLibraryBox dialog).
    // One-shot open/close commands, then live population while it is open.
    if (emit_open_library_) {
      wd.requestSubPanel(kFunctionLibraryUi);
      emit_open_library_ = false;
    }
    if (emit_close_library_) {
      wd.closeSubPanel();
      emit_close_library_ = false;
    }
    // Save-current-function dialogs (PJ3 parity). Name prompt is prefilled with the
    // current name; the overwrite warning only appears for an existing name.
    if (emit_save_name_dialog_) {
      wd.setText("saveFunctionName", pending_save_name_);
      wd.requestSubDialog(kSaveNameUi);
      emit_save_name_dialog_ = false;
    }
    if (emit_save_confirm_dialog_) {
      wd.requestSubDialog(kOverwriteUi);
      emit_save_confirm_dialog_ = false;
    }
    if (library_open_) {
      const std::vector<std::string> names = filteredSnippetNames();
      wd.setTableHeaders("tableFunctions", {"Function", "Language"});
      std::vector<std::vector<std::string>> lib_rows;
      lib_rows.reserve(names.size());
      for (const auto& n : names) {
        auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == n; });
        const std::string lang = (it != snippets_.end() && it->language == "python") ? "Python" : "Lua";
        lib_rows.push_back({n, lang});
      }
      wd.setTableRows("tableFunctions", lib_rows);
      wd.setCodeContent("previewPlainText", combinedSnippetText(library_selected_))
          .setCodeLanguage("previewPlainText", "lua");
    }

    // Import / Export library buttons: the host drives the native file choosers.
    // Import opens an "open" dialog and Export a "save as"; both report back via
    // onFileSelected, routed by widget name. Complements the library browser above.
    wd.setFilePicker("buttonLoadFunctions", "", "Snippet library (*.json)", "Import snippet library");
    wd.setSaveFilePicker("buttonSaveFunctions", "", "Snippet library (*.json)", "Export snippet library", "json");

    // Single-tab validation overlay — list every blocking problem over the chart.
    // validation_error_ is the host's real compile/run error (refreshPreview ran
    // the script as an ephemeral transform). PJ4 keeps its Modify-by-name
    // behaviour, so an already-existing name is not an error here.
    std::string single_term;
    if (output_name_.empty()) {
      single_term += "Create a name for the new series\n";
    } else if (std::find(sources_.begin(), sources_.end(), output_name_) != sources_.end()) {
      single_term += "Give the new series a name that isn't one of its inputs\n";
    }
    if (sources_.empty()) {
      single_term += "Add an input time series (drag & drop)\n";
    }
    if (function_body_.empty()) {
      single_term += "Write your function body\n";
    }
    if (!validation_error_.empty()) {
      single_term += validation_error_ + "\n";
    }
    // A one-shot Import/Export status line shows in the overlay for this render
    // (above any validation problems) and then clears on the next build.
    if (!io_status_.empty()) {
      single_term = single_term.empty() ? io_status_ : io_status_ + "\n" + single_term;
      io_status_.clear();
    }
    // Red fill on the name field when it's missing (PJ3 parity).
    wd.setFieldValid("nameLineEdit", !output_name_.empty(), output_name_.empty() ? "Name is required" : "");
    if (!single_term.empty()) {
      wd.clearChart("framePlotPreview");
      wd.setChartPlaceholder("framePlotPreview", single_term);
    } else {
      wd.setChartPlaceholder("framePlotPreview", "");
      wd.setChartSeries("framePlotPreview", preview_series_);
      wd.setChartAutoZoom("framePlotPreview", autozoom_);
    }

    // Batch tab content (set first so the validation overlay and Create gating
    // below see the current state). The rows ARE the input set, not a selection.
    // setDropTarget stays unconditional even in edit mode: the panel engine reads
    // the declared targets from the FIRST snapshot only, so gating it here would
    // install no drop filter at all — onItemsDropped does the gating instead.
    wd.setDropTarget("tableBatchSources");
    // One column, headers hidden: the header is only here to give the column a
    // width to stretch, the same way tableSources uses its own.
    wd.setTableHeaders("tableBatchSources", {"Input timeseries"});
    std::vector<std::vector<std::string>> batch_rows;
    batch_rows.reserve(batch_sources_.size());
    for (const std::string& source : batch_sources_) {
      batch_rows.push_back({source});
    }
    wd.setTableRows("tableBatchSources", batch_rows);
    wd.setListPlaceholder("tableBatchSources", "Drag & drop timeseries here");
    wd.setListItemsDeletable("tableBatchSources", !edit_mode_);
    const char* batch_lang = (batch_language_ == "python") ? "python" : "lua";
    wd.setCodeContent("globalVarsTextBatch", batch_global_code_).setCodeLanguage("globalVarsTextBatch", batch_lang);
    wd.setCodeContent("functionTextBatch", batch_function_body_).setCodeLanguage("functionTextBatch", batch_lang);
    wd.setChecked("luaBatchButton", batch_language_ != "python");
    wd.setChecked("pythonBatchButton", batch_language_ == "python");
    wd.setText("suffixLineEdit", batch_suffix_);
    wd.setChecked("radioButtonPrefix", batch_use_prefix_);
    wd.setChecked("radioButtonSuffix", !batch_use_prefix_);

    // Batch validation overlay — veil the source list with every blocking problem.
    // batch_validation_error_ is the host's real compile/run error (validateBatch).
    // The empty-input gate is NOT a batch_term: tableBatchSources renders its own
    // placeholder over the same rect. It lives on the Create setEnabled below.
    std::string batch_term;
    if (batch_function_body_.empty()) {
      batch_term += "Write your function body\n";
    }
    if (!batch_validation_error_.empty()) {
      batch_term += batch_validation_error_ + "\n";
    }
    wd.setChartPlaceholder("framePlotPreviewBatch", batch_term);

    // Each tab owns its Create action and validation gate. A pre-existing Single
    // output name and either explicit edit mode use the Modify label.
    const bool is_modify_single = outputNameExists(output_name_) || edit_mode_;
    wd.setEnabled("pushButtonCreate", single_term.empty());
    wd.setButtonText("pushButtonCreate", is_modify_single ? "Modify Time Series" : "Create New Time Series");
    // Missing affix and empty inputs gate via the disabled button, not the overlay.
    wd.setEnabled("pushButtonCreateBatch", batch_term.empty() && !batch_sources_.empty() && !batch_suffix_.empty());
    wd.setButtonText("pushButtonCreateBatch", edit_mode_ ? "Modify Time Series" : "Create New Time Series");
    // Lock the identity while editing so a rename can't fork a new series (PJ3
    // parity). Single: the name field. Batch: the sources + prefix/suffix that form
    // the name. setEnabled(false) is cosmetic for drops — see onItemsDropped.
    wd.setEnabled("nameLineEdit", !edit_mode_);
    wd.setEnabled("tableBatchSources", !edit_mode_);
    // Each Clear tracks its own list: nothing to clear, nothing to press. Batch
    // also follows the edit lock, matching its per-row trash.
    wd.setEnabled("buttonClearSources", !sources_.empty());
    wd.setEnabled("buttonClearBatchSources", !batch_sources_.empty() && !edit_mode_);
    wd.setEnabled("suffixLineEdit", !edit_mode_);
    wd.setEnabled("radioButtonPrefix", !edit_mode_);
    wd.setEnabled("radioButtonSuffix", !edit_mode_);

    return wd;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "nameLineEdit") {
      output_name_ = std::string(text);
      return true;
    }
    // Live search filter in the function library box.
    if (name == "searchLineEdit") {
      library_search_ = std::string(text);
      return true;
    }
    // Name typed in the "Save current function" prompt (harvested on OK).
    if (name == "saveFunctionName") {
      pending_save_name_ = std::string(text);
      return true;
    }
    if (name == "suffixLineEdit") {
      batch_suffix_ = std::string(text);
      return true;
    }
    return false;
  }

  bool onCodeChanged(std::string_view name, std::string_view text) override {
    if (name == "globalVarsText") {
      global_code_ = std::string(text);
      validateSyntax();
      preview_dirty_ = true;
      return true;
    }
    if (name == "functionText") {
      function_body_ = std::string(text);
      validateSyntax();
      preview_dirty_ = true;
      return true;
    }
    if (name == "globalVarsTextBatch") {
      batch_global_code_ = std::string(text);
      batch_dirty_ = true;
      return true;
    }
    if (name == "functionTextBatch") {
      batch_function_body_ = std::string(text);
      batch_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "pushButtonCreate") {
      pending_create_ = PendingCreate::Single;
      return true;
    }
    if (name == "pushButtonCreateBatch") {
      pending_create_ = PendingCreate::Batch;
      return true;
    }
    // Clear affordance in each Input Timeseries band: empty that tab's input list
    // in one go, the bulk counterpart of the per-row trash.
    if (name == "buttonClearSources") {
      sources_.clear();
      primary_index_ = -1;
      preview_dirty_ = true;
      return true;
    }
    if (name == "buttonClearBatchSources") {
      // edit_mode_ locks the input set — see onItemsDropped.
      if (edit_mode_) {
        return true;
      }
      batch_sources_.clear();
      batch_dirty_ = true;
      return true;
    }
    // Function library box (PJ3's buttonLibraryBox): open the interactive sub-panel.
    if (name == "buttonLibraryBox") {
      library_open_ = true;
      emit_open_library_ = true;
      library_search_.clear();
      library_selected_.clear();
      return true;
    }
    // "Use" in the library box: load the selected function(s) and dismiss the box.
    // There is no Close button, so Use also doubles as the way to close it.
    if (name == "useButton") {
      loadSnippetsIntoEditor(library_selected_);
      library_open_ = false;
      emit_close_library_ = true;
      return true;
    }
    // Synthetic event the host sends when the user dismisses the sub-panel.
    if (name == "subPanelClosed") {
      library_open_ = false;
      return true;
    }
    // Save current function (PJ3 parity): prompt for a name (prefilled with the
    // current one), then warn before overwriting an existing entry. Opens the
    // name-prompt modal; the actual save happens on subDialogAccepted.
    if (name == "buttonSaveCurrent") {
      pending_save_name_ = output_name_;
      save_stage_ = SaveStage::NamePrompt;
      emit_save_name_dialog_ = true;
      return true;
    }
    // A modal sub-dialog was accepted (OK). Drives the save state machine.
    if (name == "subDialogAccepted") {
      if (save_stage_ == SaveStage::NamePrompt) {
        if (pending_save_name_.empty()) {
          save_stage_ = SaveStage::None;
        } else if (snippetExists(pending_save_name_)) {
          save_stage_ = SaveStage::OverwriteConfirm;
          emit_save_confirm_dialog_ = true;  // ask before overwriting
        } else {
          doSaveSnippet(pending_save_name_);
          save_stage_ = SaveStage::None;
        }
      } else if (save_stage_ == SaveStage::OverwriteConfirm) {
        doSaveSnippet(pending_save_name_);  // user confirmed overwrite
        save_stage_ = SaveStage::None;
      }
      return true;
    }
    if (name == "pushButtonHelp") {
      help_requested_ = true;
      return true;
    }
    return false;
  }

  bool onItemDeleteRequested(std::string_view name, int index) override {
    if (name == "tableBatchSources") {
      // edit_mode_ locks the input set (see onItemsDropped). Belt-and-braces here:
      // list_deletable already hides the trash button, so this only fires if that
      // ever stops holding.
      if (edit_mode_ || index < 0 || index >= static_cast<int>(batch_sources_.size())) {
        return false;
      }
      batch_sources_.erase(batch_sources_.begin() + index);
      batch_dirty_ = true;
      return true;
    }
    if (name != "tableSources" || index < 0 || index >= static_cast<int>(sources_.size())) {
      return false;
    }

    const std::string primary_path = primarySource();
    sources_.erase(sources_.begin() + index);
    primary_index_ = 0;
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
      if (sources_[static_cast<std::size_t>(i)] == primary_path) {
        primary_index_ = i;
        break;
      }
    }
    if (sources_.empty()) {
      primary_index_ = -1;
    }
    preview_dirty_ = true;
    return true;
  }

  bool onTick() override {
    const PendingCreate action = pending_create_;
    pending_create_ = PendingCreate::None;
    if (action == PendingCreate::Single) {
      if (on_save_) {
        on_save_();
      }
    } else if (action == PendingCreate::Batch) {
      if (on_save_batch_) {
        on_save_batch_();
      }
    }
    // Always refresh the preview every tick so a live/streaming source keeps
    // moving in the chart (mirrors the FFT toolbox). refreshPreview re-reads the
    // latest samples and re-applies the Lua function each time.
    preview_dirty_ = false;
    if (on_refresh_preview_) {
      on_refresh_preview_();
    }
    // Re-validate the batch function only when its fields changed (not every tick).
    if (batch_dirty_) {
      batch_dirty_ = false;
      if (on_validate_batch_) {
        on_validate_batch_();
      }
    }
    return true;
  }

  std::string widget_data() override {
    PJ::WidgetData wd = buildWidgetData();
    if (help_requested_) {
      help_requested_ = false;
      wd.requestSubDialog(kHelpDialogUi);
    }
    if (pending_close_) {
      pending_close_ = false;
      wd.requestClose("user_closed");
    }
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "tableFunctions") {
      library_selected_ = items;
      return true;
    }
    // No tableBatchSources branch on purpose: the host clears the table before
    // repopulating it, and that clear arrives here as an empty selection — a branch
    // that stored `items` would wipe batch_sources_ one tick after every drop.
    return false;
  }

  // Radio in the sources table: make `row` the series that provides `value`.
  bool onTableRadioSelected(std::string_view name, int row) override {
    if (name == "tableSources" && row >= 0 && row < static_cast<int>(sources_.size())) {
      primary_index_ = row;
      preview_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "radioButtonPrefix") {
      batch_use_prefix_ = checked;
      return true;
    }
    if (name == "radioButtonSuffix") {
      batch_use_prefix_ = !checked;
      return true;
    }
    // Single-tab script language (Lua / Python). Only "luau" actually runs today;
    // selecting Python re-validates so the host's "unsupported language" surfaces.
    if (name == "luaButton" && checked) {
      language_ = "luau";
      validateSyntax();
      return true;
    }
    if (name == "pythonButton" && checked) {
      language_ = "python";
      validateSyntax();
      return true;
    }
    if (name == "luaBatchButton" && checked) {
      batch_language_ = "luau";
      batch_dirty_ = true;
      return true;
    }
    if (name == "pythonBatchButton" && checked) {
      batch_language_ = "python";
      batch_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onTabChanged(std::string_view name, int index) override {
    if (name == "tabWidget") {
      current_tab_ = index;
      return true;
    }
    return false;
  }

  bool onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) override {
    if (widget_name == "tableSources") {
      const bool was_empty = sources_.empty();
      for (const auto& item : items) {
        if (std::find(sources_.begin(), sources_.end(), item) == sources_.end()) {
          sources_.push_back(item);
        }
      }
      // The first series dropped becomes the primary (`value`) by default.
      if (was_empty && !sources_.empty()) {
        primary_index_ = 0;
      }
      preview_dirty_ = true;
      return true;
    }
    if (widget_name == "tableBatchSources") {
      // Identity lock: in edit mode the stored source is what names the output, so a
      // second drop would modify the original AND create another series on the same
      // click. Enforced here, not by setEnabled(false): the drop filter lives on the
      // panel root and QWidget::childAt does not skip disabled children, so a greyed
      // widget still receives drops.
      if (edit_mode_) {
        return true;
      }
      for (const auto& item : items) {
        if (std::find(batch_sources_.begin(), batch_sources_.end(), item) == batch_sources_.end()) {
          batch_sources_.push_back(item);
        }
      }
      batch_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onItemDoubleClicked(std::string_view name, int index) override {
    // Double-click in the library box: load the current selection (or the
    // double-clicked row if nothing is selected yet) and dismiss the box. The
    // index is into the FILTERED list shown in the table, not into snippets_.
    if (name == "tableFunctions") {
      std::vector<std::string> to_load = library_selected_;
      if (to_load.empty()) {
        const auto names = filteredSnippetNames();
        if (index >= 0 && index < static_cast<int>(names.size())) {
          to_load = {names[static_cast<std::size_t>(index)]};
        }
      }
      loadSnippetsIntoEditor(to_load);
      library_open_ = false;
      emit_close_library_ = true;
      return true;
    }
    return false;
  }

  // Merge `incoming` into the library by name: an incoming snippet replaces a
  // same-named one, otherwise it is appended. Existing snippets whose names are
  // absent from `incoming` are kept — so Import is additive, never destructive.
  void mergeSnippets(const std::vector<Snippet>& incoming) {
    for (const auto& s : incoming) {
      auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& e) { return e.name == s.name; });
      if (it != snippets_.end()) {
        *it = s;
      } else {
        snippets_.push_back(s);
      }
    }
  }

  // Both Import and Export report the chosen path here (the host opens an "open"
  // dialog for one and a "save as" for the other, per the widget's action). Each
  // sets io_status_, a one-shot line the next widget_data() shows then clears.
  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadFunctions") {
      // Import: merge the chosen library into the current one (additive, so the
      // user's other snippets survive) and persist so it outlives a restart.
      if (auto loaded = loadSnippetsFromPath(std::filesystem::path(path))) {
        if (loaded->empty()) {
          io_status_ = "The selected file contains no functions.";
        } else {
          mergeSnippets(*loaded);
          saveSnippetsToDisk(snippets_);
          io_status_ = "Imported " + std::to_string(loaded->size()) + " function(s).";
        }
      } else {
        io_status_ = "Could not read a function library from the selected file.";
      }
      return true;
    }
    if (widget_name == "buttonSaveFunctions") {
      // Export: write the current library to the chosen file.
      const bool ok = saveSnippetsToPath(snippets_, std::filesystem::path(path));
      io_status_ =
          ok ? "Exported " + std::to_string(snippets_.size()) + " function(s)." : "Could not write the selected file.";
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["output_name"] = output_name_;
    cfg["global_code"] = global_code_;
    cfg["function_body"] = function_body_;
    cfg["sources"] = sources_;
    cfg["primary_index"] = primaryIndex();
    cfg["language"] = language_;  // restore the Lua/Python radio on Modify
    cfg["mode"] = "single";       // reopen on the Single tab when modified (PJ3 parity)
    // Back-compat mirror: an older editor reads source_series + extra_sources, so
    // expose the primary as the source and the rest as extras in order.
    cfg["source_series"] = primarySource();
    cfg["extra_sources"] = orderedExtras();
    return cfg.dump();
  }

  // Build the editor config for ONE batch-created series so that editing it later
  // reopens the BATCH tab (PJ3 parity), repopulated with this series' source,
  // prefix/suffix, global, body and language.
  static std::string makeBatchConfig(
      const std::string& name, const std::string& global, const std::string& body, const std::string& source,
      const std::string& language, const std::string& suffix, bool use_prefix) {
    nlohmann::json cfg;
    cfg["mode"] = "batch";
    cfg["output_name"] = name;
    cfg["global_code"] = global;
    cfg["function_body"] = body;
    cfg["language"] = language;
    cfg["suffix"] = suffix;
    cfg["use_prefix"] = use_prefix;
    cfg["sources"] = std::vector<std::string>{source};
    return cfg.dump();
  }

  bool loadConfig(std::string_view json) override {
    auto cfg = nlohmann::json::parse(json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    // A batch-created series reopens the BATCH tab (PJ3 parity), repopulated with
    // its source, prefix/suffix, global, body and language.
    if (cfg.value("mode", std::string{}) == "batch") {
      batch_global_code_ = cfg.value("global_code", std::string{});
      batch_function_body_ = cfg.value("function_body", std::string{});
      batch_language_ = cfg.value("language", std::string{"luau"});
      batch_suffix_ = cfg.value("suffix", std::string{});
      batch_use_prefix_ = cfg.value("use_prefix", false);
      batch_sources_.clear();
      if (cfg.contains("sources") && cfg["sources"].is_array()) {
        batch_sources_ = cfg["sources"].get<std::vector<std::string>>();
      }
      current_tab_ = 1;             // open on the Batch tab
      pending_tab_restore_ = true;  // one-shot: push the tab to the UI
      batch_dirty_ = true;
      edit_mode_ = true;
      return true;
    }
    output_name_ = cfg.value("output_name", std::string{});
    global_code_ = cfg.value("global_code", std::string{});
    function_body_ = cfg.value("function_body", std::string{});
    language_ = cfg.value("language", std::string{"luau"});
    current_tab_ = 0;             // open on the Single tab
    pending_tab_restore_ = true;  // one-shot: push the tab to the UI
    sources_.clear();
    primary_index_ = -1;
    if (cfg.contains("sources") && cfg["sources"].is_array()) {
      sources_ = cfg["sources"].get<std::vector<std::string>>();
      primary_index_ = sources_.empty() ? -1 : cfg.value("primary_index", 0);
    } else {
      // Legacy format: one source_series (the primary) plus an extra_sources list.
      const std::string src = cfg.value("source_series", std::string{});
      if (!src.empty()) {
        sources_.push_back(src);
      }
      if (cfg.contains("extra_sources") && cfg["extra_sources"].is_array()) {
        for (auto& e : cfg["extra_sources"].get<std::vector<std::string>>()) {
          sources_.push_back(e);
        }
      }
      primary_index_ = sources_.empty() ? -1 : 0;
    }
    // Loading a populated config = editing an existing series → MODIFY mode: the
    // button reads "Modify" and the name is locked so the user can't accidentally
    // fork a new series (PJ3 parity: editExistingPlot disables nameLineEdit).
    edit_mode_ = !output_name_.empty();
    return true;
  }

  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }
  void setOnSaveBatch(std::function<void()> cb) {
    on_save_batch_ = std::move(cb);
  }
  void setOnRefreshPreview(std::function<void()> cb) {
    on_refresh_preview_ = std::move(cb);
  }
  void setOnValidateBatch(std::function<void()> cb) {
    on_validate_batch_ = std::move(cb);
  }
  void setSnippets(std::vector<Snippet> s) {
    snippets_ = std::move(s);
  }

  bool snippetExists(const std::string& snippet_name) const {
    return std::any_of(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == snippet_name; });
  }

  // Save the current editor contents under `snippet_name` (insert or overwrite),
  // then persist the library to disk. The name prompt + overwrite warning are
  // handled by the caller (the save state machine), mirroring PJ3.
  void doSaveSnippet(const std::string& snippet_name) {
    Snippet sn{snippet_name, global_code_, function_body_, language_};
    auto it =
        std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == snippet_name; });
    if (it != snippets_.end()) {
      *it = sn;
    } else {
      snippets_.push_back(sn);
    }
    saveSnippetsToDisk(snippets_);
  }

  // Snippet names matching the library box's search filter (case-insensitive
  // substring), in library order. Shared by the table population and the
  // double-click handler so the displayed-row index maps to the right snippet.
  std::vector<std::string> filteredSnippetNames() const {
    auto lower = [](std::string v) {
      for (char& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return v;
    };
    const std::string q = lower(library_search_);
    std::vector<std::string> out;
    for (const auto& s : snippets_) {
      if (q.empty() || lower(s.name).find(q) != std::string::npos) {
        out.push_back(s.name);
      }
    }
    return out;
  }

  // Render the combined code of the given library functions (globals first, then
  // the function body) — used both for the box preview and for what Use loads.
  // Multiple selections are concatenated in order, mirroring PJ3's combine.
  std::string combinedSnippetText(const std::vector<std::string>& names) const {
    std::string globals;
    std::string body;
    for (const auto& n : names) {
      auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == n; });
      if (it == snippets_.end()) {
        continue;
      }
      if (!it->global_code.empty()) {
        if (!globals.empty()) {
          globals += "\n";
        }
        globals += it->global_code;
      }
      if (!body.empty()) {
        body += "\n";
      }
      body += it->function_body;
    }
    if (globals.empty()) {
      return body;
    }
    return globals + "\n\nfunction(time,value)\n" + body + "\nend";
  }

  // Load the selected library function(s) into the Single-tab editor (combined
  // globals + body), then re-validate. Mirrors PJ3's "Use" / double-click.
  void loadSnippetsIntoEditor(const std::vector<std::string>& names) {
    if (names.empty()) {
      return;
    }
    std::string globals;
    std::string body;
    bool language_set = false;
    for (const auto& n : names) {
      auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == n; });
      if (it == snippets_.end()) {
        continue;
      }
      // Adopt the first snippet's language so the editor interprets it correctly
      // (and flips the Lua/Python radio). A multi-select combine uses the first.
      if (!language_set) {
        language_ = it->language.empty() ? "luau" : it->language;
        language_set = true;
      }
      if (!it->global_code.empty()) {
        if (!globals.empty()) {
          globals += "\n";
        }
        globals += it->global_code;
      }
      if (!body.empty()) {
        body += "\n";
      }
      body += it->function_body;
    }
    global_code_ = globals;
    function_body_ = body;
    output_name_ = names.front();  // seed with the first; the user can rename
    validateSyntax();
  }

  void setPreviewSeries(std::vector<PJ::ChartSeries> series) {
    preview_series_ = std::move(series);
  }
  /// Set by the toolbox after each ephemeral-preview attempt: empty = script
  /// accepted by the host; non-empty = the error shown over the preview chart.
  void setValidationError(std::string error) {
    validation_error_ = std::move(error);
  }
  /// Ask the host to close this panel (e.g. after a successful Create — matches PJ3,
  /// where creating the series closes the editor). Honoured on the next widget_data().
  void requestClose() {
    pending_close_ = true;
  }
  /// Batch-tab counterpart of setValidationError (shown over the source list).
  void setBatchValidationError(std::string error) {
    batch_validation_error_ = std::move(error);
  }
  /// Current batch function body / globals (read by the toolbox to validate).
  const std::string& batchFunctionBody() const {
    return batch_function_body_;
  }
  const std::string& batchGlobalCode() const {
    return batch_global_code_;
  }
  /// Active tab, used to suspend the Single preview while Batch is visible.
  int currentTab() const {
    return current_tab_;
  }
  const std::vector<std::string>& batchSources() const {
    return batch_sources_;
  }
  const std::string& batchSuffix() const {
    return batch_suffix_;
  }
  bool batchUsePrefix() const {
    return batch_use_prefix_;
  }
  /// True once since the last consumeBatchDirty(); set on any batch field change.
  bool consumeBatchDirty() {
    const bool was = batch_dirty_;
    batch_dirty_ = false;
    return was;
  }
  void setAvailableSeries(std::vector<std::string> series) {
    all_series_ = std::move(series);
  }

  // The series that provides `value` (the radio-selected row), "" when no inputs.
  std::string sourceSeries() const {
    return primarySource();
  }
  const std::string& outputName() const {
    return output_name_;
  }
  const std::string& globalCode() const {
    return global_code_;
  }
  const std::string& functionBody() const {
    return function_body_;
  }
  const std::string& language() const {
    return language_;
  }
  const std::string& batchLanguage() const {
    return batch_language_;
  }
  // The v1, v2, ... inputs (non-primary rows, in row order).
  std::vector<std::string> extraSources() const {
    return orderedExtras();
  }

 private:
  // True if a series with output name `name` already exists (so a Create would
  // replace it → the button shows "Modify"). Matches against the available-series
  // list, both as a full path and as the last "/"-segment.
  bool outputNameExists(const std::string& name) const {
    if (name.empty()) {
      return false;
    }
    for (const auto& s : all_series_) {
      if (s == name) {
        return true;
      }
      // `name` is the output TOPIC; the series is listed as "topic/field".
      if (s.size() > name.size() && s.compare(0, name.size(), name) == 0 && s[name.size()] == '/') {
        return true;
      }
      // `name` matches the last path segment (field).
      const auto slash = s.find_last_of('/');
      if (slash != std::string::npos && s.substr(slash + 1) == name) {
        return true;
      }
    }
    return false;
  }

  void validateSyntax() {
    // Lightweight check: non-empty function body = tentatively OK
    syntax_ok_ = !function_body_.empty();
  }

  // Clamped primary row: the checked radio, or row 0 when the stored index is
  // stale (e.g. the primary row was just deleted), or -1 when there are no inputs.
  int primaryIndex() const {
    if (sources_.empty()) {
      return -1;
    }
    if (primary_index_ < 0 || primary_index_ >= static_cast<int>(sources_.size())) {
      return 0;
    }
    return primary_index_;
  }

  // The series that provides `value` ("" when there are no inputs).
  std::string primarySource() const {
    const int p = primaryIndex();
    return p < 0 ? std::string{} : sources_[static_cast<std::size_t>(p)];
  }

  // Non-primary series in row order — the v1, v2, ... inputs.
  std::vector<std::string> orderedExtras() const {
    std::vector<std::string> extras;
    const int p = primaryIndex();
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
      if (i != p) {
        extras.push_back(sources_[static_cast<std::size_t>(i)]);
      }
    }
    return extras;
  }

  // Variable name shown for row `i`: "value" for the primary, else v1, v2, ... by
  // the row's position among the non-primary rows (matches buildTransformScript).
  std::string variableName(int i) const {
    const int p = primaryIndex();
    if (i == p) {
      return "value";
    }
    int vnum = 0;
    for (int j = 0; j <= i; ++j) {
      if (j != p) {
        ++vnum;
      }
    }
    return "v" + std::to_string(vnum);
  }

  // Single function state. All dragged inputs live in one table; the row whose
  // radio is checked (`primary_index_`) provides `value`, and the rest become
  // v1, v2, ... in row order.
  std::string language_ = "luau";        // single-tab script language: "luau" | "python" (radios)
  std::string batch_language_ = "luau";  // batch-tab script language (radios)
  std::string output_name_;
  std::string global_code_;
  std::string function_body_ = "return value";
  std::vector<std::string> sources_;
  int primary_index_ = -1;
  bool syntax_ok_ = false;
  bool autozoom_ = true;                // AutoZoom checkbox state (default on)
  bool edit_mode_ = false;              // opened to modify an existing series (locks the name, button = Modify)
  std::string validation_error_;        // host's rejection message for the current script (empty = OK)
  std::string batch_validation_error_;  // batch-tab counterpart (empty = OK)
  bool batch_dirty_ = true;             // batch script/inputs changed → re-validate (start dirty)

  // Batch state
  std::string batch_global_code_;
  std::string batch_function_body_ = "return value";
  std::string batch_suffix_;
  int current_tab_ = 0;                     // active tab (0 = Single, 1 = Batch)
  bool pending_tab_restore_ = false;        // one-shot: push the tab after loadConfig
  std::vector<std::string> batch_sources_;  // full paths dropped into tableBatchSources
  bool batch_use_prefix_ = false;           // Prefix vs Suffix radio (default Suffix)
  std::vector<std::string> all_series_;     // live catalog, for outputNameExists

  // Library
  std::vector<Snippet> snippets_;
  // Function library box (interactive sub-panel) state.
  bool library_open_ = false;                  // sub-panel currently shown
  bool emit_open_library_ = false;             // one-shot: emit requestSubPanel next build
  bool emit_close_library_ = false;            // one-shot: emit closeSubPanel next build
  std::string library_search_;                 // current search-filter text
  std::vector<std::string> library_selected_;  // names of the rows selected in the box

  // "Save current function" flow (PJ3 parity): name prompt -> overwrite warning.
  enum class SaveStage { None, NamePrompt, OverwriteConfirm };
  SaveStage save_stage_ = SaveStage::None;
  std::string pending_save_name_;          // name being saved (from the prompt)
  bool emit_save_name_dialog_ = false;     // one-shot: open the name prompt
  bool emit_save_confirm_dialog_ = false;  // one-shot: open the overwrite warning

  std::string io_status_;  // one-shot Import/Export status line; consumed by the next widget_data()

  enum class PendingCreate { None, Single, Batch };
  PendingCreate pending_create_ = PendingCreate::None;
  bool pending_close_ = false;
  bool preview_dirty_ = false;
  bool help_requested_ = false;
  std::function<void()> on_save_;
  std::function<void()> on_save_batch_;
  std::function<void()> on_refresh_preview_;
  std::function<void()> on_validate_batch_;
  std::vector<PJ::ChartSeries> preview_series_;
};

// ---------------------------------------------------------------------------
// TransformEditorToolbox
// ---------------------------------------------------------------------------

class TransformEditorToolbox : public PJ::ToolboxPluginBase {
 public:
  // The editor has no Close button; it is dismissed via the host panel chrome,
  // which destroys this plugin instance. Remove the ephemeral live-preview node
  // here so it cannot keep recomputing in the DerivedEngine after the editor is
  // gone (tearDownPreview() is a no-op when no preview is live).
  ~TransformEditorToolbox() override {
    tearDownPreview();
  }

  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    if (!callbacks_wired_) {
      dialog_.setOnSave([this]() { onSave(); });
      dialog_.setOnSaveBatch([this]() { onSaveBatch(); });
      dialog_.setOnRefreshPreview([this]() { refreshPreview(); });
      dialog_.setOnValidateBatch([this]() { validateBatch(); });
      callbacks_wired_ = true;
    }
    dialog_.setSnippets(loadSnippetsFromDisk());

    refreshPreview();
    return PJ::borrowDialog(dialog_);
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    // Request the data processors host bridge for live (DerivedEngine) transforms.
    if (auto dp = services.get<PJ::sdk::DataProcessorsHostService>()) {
      dp_view_ = *dp;
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view json) override {
    if (!dialog_.loadConfig(json)) {
      return PJ::unexpected("invalid transform editor config");
    }
    return PJ::okStatus();
  }

 private:
  void onSave() {
    const auto& source = dialog_.sourceSeries();
    const auto& output_name = dialog_.outputName();
    const auto& global = dialog_.globalCode();
    const auto& body = dialog_.functionBody();

    const auto report = [this](PJ::ToolboxMessageLevel level, const std::string& msg) {
      if (runtimeHostBound()) {
        runtimeHost().reportMessage(level, msg);
      }
    };

    if (source.empty() || output_name.empty() || body.empty()) {
      report(
          PJ::ToolboxMessageLevel::kWarning,
          "Transform Editor: a source series, an output name, and a function body are required.");
      return;
    }
    if (!dp_view_.valid()) {
      report(
          PJ::ToolboxMessageLevel::kError,
          "Transform Editor: the host did not expose pj.data_processors.v1 (cannot create the series).");
      return;
    }

    // Inputs: the principal source first, then the additional sources as v1..vN.
    // Pass the FULL field path ("dummy/noise/random"); the host resolves it to the
    // owning topic AND the selected leaf column, so the transform reads the chosen
    // field instead of always column 0 of a multi-field topic.
    // The host runs N->1: a single input installs a SISO node, multiple inputs a
    // MIMO node. The synthesized class arity matches the input count.
    std::vector<std::string> input_topics;
    input_topics.push_back(source);
    for (const std::string& extra : dialog_.extraSources()) {
      input_topics.push_back(extra);
    }
    const std::size_t num_extra = dialog_.extraSources().size();

    // The name field doubles as a comma-separated list of output topics; the body
    // must `return` one value per declared name (positional). See splitOutputNames.
    std::vector<std::string> output_names = splitOutputNames(output_name);
    if (output_names.empty()) {
      report(PJ::ToolboxMessageLevel::kWarning, "Transform Editor: no valid output name.");
      return;
    }

    // Build a self-describing Luau filter class and hand it to the host. The host
    // compiles + runs it as an eager DerivedEngine node that RECOMPUTES LIVE as new
    // samples arrive — so a streaming source produces a streaming output that
    // survives the plugin/panel closing.
    const std::string id = output_names.front();  // host namespaces it under the plugin id
    const std::string script =
        buildTransformScript(id, output_names.front(), global, body, num_extra, dialog_.language());

    std::vector<std::string_view> inputs(input_topics.begin(), input_topics.end());
    std::vector<std::string_view> outputs(output_names.begin(), output_names.end());
    // Persist the editor state in params_json so the Edit (pencil) button can
    // reconstruct the editor for an in-place Modify. The script ignores params.
    const std::string editor_params = dialog_.saveConfig();
    auto status = dp_view_.createTransform(
        id, PJ::Span<const std::string_view>(inputs.data(), inputs.size()),
        PJ::Span<const std::string_view>(outputs.data(), outputs.size()), script, editor_params);
    if (!status) {
      report(PJ::ToolboxMessageLevel::kError, "Transform Editor: " + std::string(status.error()));
      return;
    }
    report(
        PJ::ToolboxMessageLevel::kInfo,
        "Transform Editor: created " + std::to_string(output_names.size()) + " series.");
    // The host materializes the topic; refresh so it shows in the Custom Series panel.
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    dialog_.requestClose();  // PJ3 parity: creating the series closes the editor.
  }

  // Batch create: apply the batch function to EVERY input series, naming each
  // output with the prefix/suffix (mirrors the native panel's batch path). One
  // transform per source; each output is surfaced in Custom Series via on_data_changed.
  void onSaveBatch() {
    const auto report = [this](PJ::ToolboxMessageLevel level, const std::string& msg) {
      if (runtimeHostBound()) {
        runtimeHost().reportMessage(level, msg);
      }
    };
    const std::string& body = dialog_.batchFunctionBody();
    const std::string& global = dialog_.batchGlobalCode();
    const std::string& suffix = dialog_.batchSuffix();
    const bool use_prefix = dialog_.batchUsePrefix();
    const auto& sources = dialog_.batchSources();
    if (body.empty() || sources.empty() || suffix.empty()) {
      report(
          PJ::ToolboxMessageLevel::kWarning,
          "Transform Editor: drop in some input series, set a prefix/suffix, and write a function body.");
      return;
    }
    if (!dp_view_.valid()) {
      report(
          PJ::ToolboxMessageLevel::kError,
          "Transform Editor: the host did not expose pj.data_processors.v1 (cannot create the series).");
      return;
    }
    int created = 0;
    for (const std::string& source_display : sources) {
      // Add the prefix/suffix to the FULL source name so each output stays unique.
      // Deriving the base from only the leaf ("value" from "test/sin/value")
      // collapses distinct sources that share a leaf name — e.g. every ".../x" tf
      // field — into one output name, so all but the first createTransform fails
      // with "structural replacement is not supported" and the editor never closes.
      const std::string name = use_prefix ? (suffix + source_display) : (source_display + suffix);
      if (name.empty()) {
        continue;
      }
      // Pass the FULL field path; the host resolves topic + selected leaf column.
      const std::string script = buildTransformScript(name, name, global, body, 0, dialog_.batchLanguage());
      std::vector<std::string_view> ins{source_display};
      std::vector<std::string_view> outs{name};
      // Persist this series' editor state (single-tab shape) so the Edit (pencil)
      // button can Modify it just like a single-created series — PJ3 parity.
      const std::string editor_params = TransformEditorDialog::makeBatchConfig(
          name, global, body, source_display, dialog_.batchLanguage(), suffix, use_prefix);
      auto status = dp_view_.createTransform(
          name, PJ::Span<const std::string_view>(ins.data(), ins.size()),
          PJ::Span<const std::string_view>(outs.data(), outs.size()), script, editor_params);
      if (!status) {
        report(
            PJ::ToolboxMessageLevel::kError,
            "Transform Editor: error on '" + source_display + "': " + std::string(status.error()));
        return;
      }
      ++created;
    }
    report(PJ::ToolboxMessageLevel::kInfo, "Transform Editor: created " + std::to_string(created) + " series.");
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    dialog_.requestClose();  // PJ3 parity: creating the series closes the editor.
  }

  void tearDownPreview() {
    if (!preview_key_.empty() && dp_view_.valid()) {
      (void)dp_view_.remove(preview_key_);
      preview_key_.clear();
    }
  }

  // Read a datastore field by name (exact, or topic/field suffix/prefix match) and
  // return its (absolute_ns, value) samples, downsampled to ~2000 points for the
  // preview. The ephemeral transform output lives in the engine's topic list (and
  // hence in catalogSnapshot) even though it is kept out of the UI catalog, so this
  // resolves both the source AND the transformed result by name.
  // Decimated (timestamp, value) samples for one already-resolved field handle.
  std::vector<std::pair<int64_t, double>> samplesFromHandle(PJ::sdk::FieldHandle handle) {
    std::vector<std::pair<int64_t, double>> out;
    auto read = toolboxHost().readSeries(handle);
    if (!read || read->rowCount() == 0 || read->valuesAsFloat64() == nullptr) {
      return out;
    }
    const auto ts = read->timestamps();
    const double* vals = read->valuesAsFloat64();
    const size_t n = read->rowCount();
    const size_t step = (n > 2000) ? (n / 2000) : 1;
    out.reserve(n / step + 1);
    for (size_t i = 0; i < n; i += step) {
      out.emplace_back(ts[i], vals[i]);
    }
    return out;
  }

  // Resolve several "topic/field" names (or a bare topic / bare field) against ONE
  // catalog snapshot in a single nested pass, returning decimated samples per name
  // (empty if a name did not resolve). The MIMO preview reads the source ghost + M
  // outputs every tick; a per-name readRawSamples would re-acquire the snapshot and
  // re-scan the whole catalog M+1 times. Field paths in the snapshot are RELATIVE to
  // their topic and a ROS leaf carries a leading '/', so reconstruct "topic/field"
  // WITHOUT doubling the slash (matching the host's joinTopicField) — a naive
  // `tname + "/" + fname` would rebuild the old "topic//field" and fail to match.
  std::vector<std::vector<std::pair<int64_t, double>>> readManyRawSamples(const std::vector<std::string>& names) {
    std::vector<std::vector<std::pair<int64_t, double>>> out(names.size());
    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) {
      return out;
    }
    const auto fields = catalog->fields();
    const auto topics = catalog->topics();
    std::vector<PJ::sdk::FieldHandle> handles(names.size());
    std::vector<bool> found(names.size(), false);
    std::size_t remaining = names.size();
    for (const auto& t : topics) {
      if (remaining == 0) {
        break;
      }
      const std::string tname(t.name.data, t.name.size);
      const uint32_t end = t.first_field + t.field_count;
      for (uint32_t fi = t.first_field; fi < end && fi < fields.size(); ++fi) {
        const auto& f = fields[fi];
        const std::string fname(f.name.data, f.name.size);
        const std::string leaf = (!fname.empty() && fname.front() == '/') ? fname.substr(1) : fname;
        const std::string full = tname.empty() ? leaf : (tname + "/" + leaf);
        for (std::size_t i = 0; i < names.size(); ++i) {
          if (found[i] || (tname != names[i] && full != names[i] && fname != names[i] && leaf != names[i])) {
            continue;
          }
          handles[i] = PJ::sdk::FieldHandle{f.handle};
          found[i] = true;
          --remaining;
        }
      }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (found[i]) {
        out[i] = samplesFromHandle(handles[i]);
      }
    }
    return out;
  }

  // Snapshot the LIVE catalog as full "topic/field" paths, skipping our own "__"
  // ephemeral outputs. Called every tick so series from a file/stream loaded after
  // the editor opened are accounted for. Feeds outputNameExists, which is what
  // decides whether the Single tab's button says Create or Modify.
  void refreshAvailableSeries() {
    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) {
      return;
    }
    const auto fields = catalog->fields();
    std::vector<std::string> series;
    for (const auto& t : catalog->topics()) {
      const std::string tname(t.name.data, t.name.size);
      if (tname.rfind("__", 0) == 0) {
        continue;
      }
      const uint32_t end = t.first_field + t.field_count;
      for (uint32_t fi = t.first_field; fi < end && fi < fields.size(); ++fi) {
        const auto& f = fields[fi];
        const std::string fname(f.name.data, f.name.size);
        // A ROS leaf carries a leading '/', so join WITHOUT doubling the slash —
        // matching the host's joinTopicField (see readManyRawSamples). A naive
        // `tname + "/" + fname` produces "topic//field", which then fails to
        // resolve when createTransform is handed it as the batch input path.
        const std::string leaf = (!fname.empty() && fname.front() == '/') ? fname.substr(1) : fname;
        series.push_back(tname.empty() ? leaf : (tname + "/" + leaf));
      }
    }
    dialog_.setAvailableSeries(std::move(series));
  }

  void refreshPreview() {
    // Only the Create/Modify label consults the catalog mirror, and that lookup
    // short-circuits on an empty name — so skip the snapshot entirely until there
    // is a name to match. Keeps the Batch tab, which never fills that field, off a
    // lock-held per-tick copy of every topic and field.
    if (!dialog_.outputName().empty()) {
      refreshAvailableSeries();
    }
    // The live preview belongs to the Single Function tab only. On the Batch tab
    // tear it down so its ephemeral node can never coexist with / leak into a
    // batch Create (and to avoid needless per-tick churn).
    if (dialog_.currentTab() != 0) {
      tearDownPreview();
      dialog_.setPreviewSeries({});
      return;
    }
    if (!dp_view_.valid()) {
      return;
    }
    const auto& source = dialog_.sourceSeries();
    const auto& global = dialog_.globalCode();
    const auto& body = dialog_.functionBody();

    if (source.empty() || body.empty()) {
      tearDownPreview();
      dialog_.setPreviewSeries({});
      dialog_.setValidationError("");  // incomplete input is not an error
      return;
    }

    // Pass the FULL field path as the input; the host resolves it to the owning
    // topic AND the selected leaf column (so the preview reads the chosen field,
    // not column 0 of a multi-field topic — matching the live Create path).
    const std::size_t num_extra = dialog_.extraSources().size();

    std::vector<std::string> input_topics;
    input_topics.push_back(source);
    for (const std::string& extra : dialog_.extraSources()) {
      input_topics.push_back(extra);
    }

    // Declare the SAME output topics the real Create path uses (splitOutputNames of
    // the comma-separated name field) so a MIMO body returning M values matches the
    // node's arity — a single-output preview node drops every row of an M-output
    // function and falsely reports "no output". Each slot gets a unique ephemeral
    // name; fall back to one slot while the name field is still empty (mid-typing).
    const std::string preview_id(kPreviewId);
    std::vector<std::string> output_names = splitOutputNames(dialog_.outputName());
    const std::size_t num_outputs = output_names.empty() ? 1 : output_names.size();
    std::vector<std::string> preview_outputs;
    preview_outputs.reserve(num_outputs);
    for (std::size_t k = 0; k < num_outputs; ++k) {
      preview_outputs.push_back(preview_id + "_" + std::to_string(k));
    }
    const std::string script =
        buildTransformScript(preview_id, preview_id, global, body, num_extra, dialog_.language());

    // Evaluate the code FIRST via the SDK (cheap: compile + one test point, no node).
    // Only materialise the live preview node when the script is valid — so a broken
    // function does not create/tear down a transform on every keystroke. The
    // validation script uses the host's expected "__validate__" class id (the
    // preview/create script keeps preview_id for the upsert).
    const std::string validate_script =
        buildTransformScript("__validate__", "__validate__", global, body, num_extra, dialog_.language());
    auto validation = dp_view_.validateScript("transform", dialog_.language(), validate_script);
    dialog_.setValidationError(validation ? "" : std::string(validation.error()));
    if (!validation) {
      tearDownPreview();
      dialog_.setPreviewSeries({});
      return;
    }

    std::vector<std::string_view> inputs_sv(input_topics.begin(), input_topics.end());
    std::vector<std::string_view> outputs_sv(preview_outputs.begin(), preview_outputs.end());

    tearDownPreview();
    auto status = dp_view_.createEphemeralTransform(
        kPreviewId, PJ::Span<const std::string_view>(inputs_sv.data(), inputs_sv.size()),
        PJ::Span<const std::string_view>(outputs_sv.data(), outputs_sv.size()), script, "{}");

    if (!status) {
      dialog_.setValidationError(std::string(status.error()));
      dialog_.setPreviewSeries({});
      return;
    }
    preview_key_ = preview_id;

    // Read the source (ghost) and each materialised output back from the datastore,
    // and hand the host explicit points to plot. Resolve all of them in ONE catalog
    // pass (readManyRawSamples) — a per-series read would re-scan the whole catalog
    // M+1 times per tick. All share a common t0 (the earliest sample across ghost +
    // outputs) so they align on the time axis.
    std::vector<std::string> read_names;
    read_names.reserve(1 + preview_outputs.size());
    read_names.push_back(source);
    read_names.insert(read_names.end(), preview_outputs.begin(), preview_outputs.end());
    auto samples = readManyRawSamples(read_names);

    auto ghost_raw = std::move(samples[0]);
    std::vector<std::vector<std::pair<int64_t, double>>> results(
        std::make_move_iterator(samples.begin() + 1), std::make_move_iterator(samples.end()));
    bool any_result = false;
    for (const auto& r : results) {
      any_result = any_result || !r.empty();
    }

    // The script compiled, but if the source has data and NONE of the M declared
    // outputs produced a row, the function never returns numbers (e.g. `return valu`)
    // — flag it. Fire ONLY when every output is empty: one empty channel among several
    // is legitimate (a suppressed output). validateScript (compile-only) can't catch
    // this runtime case; real inputs are used, so a valid MIMO is not falsely flagged.
    if (!ghost_raw.empty() && !any_result) {
      // The output-name field declares N outputs; the body must return EXACTLY N
      // values per point (positional). A silent MIMO no-op is almost always this
      // arity mismatch — in EITHER direction (too few OR too many returns) — or a
      // body that returns nothing. State the declared count symmetrically rather
      // than a misleading "must return a number" (the user may have returned several).
      const std::string n = std::to_string(num_outputs);
      const std::string plural = num_outputs == 1 ? "" : "s";
      const std::string msg = "function produced no output — declared " + n + " output" + plural +
                              ", so it must return exactly " + n + " value" + plural + " per point";
      dialog_.setValidationError(msg);
    }

    int64_t t0 = 0;
    bool have_t0 = false;
    if (!ghost_raw.empty()) {
      t0 = ghost_raw.front().first;
      have_t0 = true;
    }
    for (const auto& raw : results) {
      if (!raw.empty() && (!have_t0 || raw.front().first < t0)) {
        t0 = raw.front().first;
        have_t0 = true;
      }
    }

    const auto to_points = [t0](const std::vector<std::pair<int64_t, double>>& raw) {
      std::vector<PJ::ChartPoint> pts;
      pts.reserve(raw.size());
      for (const auto& [ts, v] : raw) {
        pts.push_back({static_cast<double>(ts - t0) / 1e9, v});
      }
      return pts;
    };

    std::vector<PJ::ChartSeries> series;
    // TODO(theme): These data-series colors are the approved color-as-data
    // exception from visual_guidelines.md 4.6.2; use renderer-selected colors
    // when the chart protocol exposes them.
    // Ghost (original): faded blue + dashed, distinct from the solid result curves
    // — same styling as the native TransformEditorPanel. Color hex is #AARRGGBB.
    series.push_back({source, to_points(ghost_raw), "#5A4488ff", /*dashed=*/true});
    // One solid curve per declared output, cycling a small palette so parallel MIMO
    // outputs stay visually distinct; labelled by the user's output name.
    static constexpr const char* kResultColors[] = {"#ff8800", "#0088ff", "#22aa22", "#cc2222", "#9933cc", "#00a0a0"};
    static constexpr std::size_t kNumColors = sizeof(kResultColors) / sizeof(kResultColors[0]);
    for (std::size_t k = 0; k < results.size(); ++k) {
      if (results[k].empty()) {
        continue;
      }
      std::string label;
      if (k < output_names.size() && !output_names[k].empty()) {
        label = output_names[k];
      } else {
        label = results.size() > 1 ? ("result" + std::to_string(k)) : std::string("result");
      }
      series.push_back({label, to_points(results[k]), kResultColors[k % kNumColors], /*dashed=*/false});
    }
    dialog_.setPreviewSeries(std::move(series));
  }

  // Validate the BATCH function via the SDK's validateScript — the host compiles
  // it and runs a synthetic test point, with NO node/topic created (unlike the old
  // throwaway-transform approach). Drives the source-list error overlay; called
  // only when the batch fields changed (consumeBatchDirty), not every tick.
  void validateBatch() {
    if (!dp_view_.valid()) {
      return;
    }
    const std::string& body = dialog_.batchFunctionBody();
    if (body.empty()) {
      dialog_.setBatchValidationError("");
      return;
    }
    const std::string script = buildTransformScript(
        "__validate__", "__validate__", dialog_.batchGlobalCode(), body, 0, dialog_.batchLanguage());
    auto status = dp_view_.validateScript("transform", dialog_.batchLanguage(), script);
    dialog_.setBatchValidationError(status ? "" : std::string(status.error()));
  }

  static constexpr std::string_view kPreviewId = "__te_preview__";

  TransformEditorDialog dialog_;
  bool callbacks_wired_ = false;
  std::optional<PJ::sdk::DataSourceHandle> ds_handle_{std::nullopt};
  PJ::sdk::DataProcessorsHostView dp_view_;
  std::string preview_key_;  // non-empty when an ephemeral preview node is live
};

}  // namespace

PJ_TOOLBOX_PLUGIN(TransformEditorToolbox, kTransformEditorManifest)
PJ_DIALOG_PLUGIN(TransformEditorDialog, kTransformEditorManifest)
