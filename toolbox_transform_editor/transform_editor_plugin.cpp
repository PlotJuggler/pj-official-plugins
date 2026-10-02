// SPDX-License-Identifier: MPL-2.0
//
// Transform Editor toolbox plugin for PlotJuggler 4.
// Ports the PJ3 "Custom Series" / Function Editor to the PJ4 semantic UI.
// Motor: pj.data_processors.v1 — onSave hands the host a self-describing Luau class
// (N inputs -> M outputs) run live as a DerivedEngine node. Requires SDK >= 0.12.0.
// Preview uses createEphemeralTransform (SDK 0.12+) — no local Lua runtime.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "derived_recipes/recipes.hpp"
#include "transform_editor_dialog_ui.hpp"
#include "transform_editor_manifest.hpp"
// Auxiliary dialog/panel UIs, embedded from their .ui files at build time:
//   kHelpDialogUi       — Help (requestSubDialog), same for both tabs.
//   kFunctionLibraryUi  — live Function Library sub-panel (requestSubPanel).
//   kSaveNameUi         — "save current function" name prompt (requestSubDialog).
//   kOverwriteUi        — overwrite-existing-function confirmation (requestSubDialog).
//   kCreateRecipeUi     — "Create..." name prompt with the summary of what is created (requestSubDialog).
#include "create_recipe_ui.hpp"
#include "function_library_ui.hpp"
#include "overwrite_function_ui.hpp"
#include "save_function_name_ui.hpp"
#include "transform_editor_help_ui.hpp"

namespace {

// ---------------------------------------------------------------------------
// Snippet
// ---------------------------------------------------------------------------

// One input a library function expects: the variable its body reads and the builtin object type
// ("kPointCloud", ...) that variable holds.
struct SnippetInput {
  std::string var;
  std::string type;
};

struct Snippet {
  std::string name;
  std::string global_code;
  std::string function_body;
  std::string language = "luau";  // "luau" | "python"; legacy/builtin snippets are Luau
  std::string kind = "series";    // "series" (time, value, v1..) | "object" (one variable per input)
  std::vector<SnippetInput> inputs = {};
  std::string description = {};
};

std::filesystem::path snippetLibraryPath() {
  return PJ::sdk::userDataDir() / "toolbox_transform_editor" / "snippets.json";
}

// A string member of `item`, or `fallback` when it is missing or not a string (a hand-edited or
// older library must never throw).
std::string jsonString(const nlohmann::json& item, const char* key, const std::string& fallback = {}) {
  const auto it = item.find(key);
  return it != item.end() && it->is_string() ? it->get<std::string>() : fallback;
}

nlohmann::json snippetToJson(const Snippet& s) {
  nlohmann::json inputs = nlohmann::json::array();
  for (const auto& input : s.inputs) {
    inputs.push_back({{"var", input.var}, {"type", input.type}});
  }
  return {
      {"name", s.name}, {"global_code", s.global_code}, {"function_body", s.function_body}, {"language", s.language},
      {"kind", s.kind}, {"inputs", std::move(inputs)},  {"description", s.description}};
}

// Tolerant of older libraries: the kind, inputs and description default when absent or malformed.
Snippet snippetFromJson(const nlohmann::json& item) {
  Snippet s;
  s.name = jsonString(item, "name");
  s.global_code = jsonString(item, "global_code");
  s.function_body = jsonString(item, "function_body");
  s.language = jsonString(item, "language", "luau");
  s.kind = jsonString(item, "kind", "series") == "object" ? "object" : "series";
  s.description = jsonString(item, "description");
  const auto inputs = item.find("inputs");
  if (inputs != item.end() && inputs->is_array()) {
    for (const auto& input : *inputs) {
      if (input.is_object() && !jsonString(input, "var").empty()) {
        s.inputs.push_back({jsonString(input, "var"), jsonString(input, "type")});
      }
    }
  }
  return s;
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
      j.push_back(snippetToJson(s));
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

// A built-in function of the object kind: its body reads one variable per declared input.
Snippet objectSnippet(
    std::string name, std::string description, std::vector<SnippetInput> inputs, std::string function_body) {
  Snippet s;
  s.name = std::move(name);
  s.function_body = std::move(function_body);
  s.kind = "object";
  s.inputs = std::move(inputs);
  s.description = std::move(description);
  return s;
}

// The built-in functions that work on objects (point clouds, images, ...), the demo recipes
// rewritten so each input is a variable and every parameter is a local with its default.
std::vector<Snippet> objectDefaultSnippets() {
  return {
      objectSnippet(
          "points_per_frame", "Number of points in each frame of a point cloud.", {{"cloud", "kPointCloud"}},
          "return cloud:count()"),
      objectSnippet(
          "lidar_crop",
          "Crops a point cloud to a box (+-10 m in x and y, -2 to 3 m in z) and marks its highest point. Outputs: "
          "the cropped cloud, its highest z, a report and a scene with a sphere on the highest point.",
          {{"cloud", "kPointCloud"}},
          R"lua(local half, zmin, zmax = 10, -2, 3
local cropped = cloud:crop_box{min = {-half, -half, zmin}, max = {half, half, zmax}}
local top = cropped:extreme_point("z", "max")
if top == nil then
  return { cropped = cropped, max_z = pj.unavailable("no points in the box"), report = "empty",
           witness = pj.scene.new{frame_id = cloud.frame_id, id = "highest"}:finish() }
end
local w = pj.scene.new{frame_id = cloud.frame_id, id = "highest"}
  :sphere{center = {top.x, top.y, top.z}, diameter = 0.3, color = {255, 40, 40, 255}}
return { cropped = cropped, max_z = top.z,
         report = string.format("%d of %d points kept; max z %.2f", cropped:count(), cloud:count(), top.z),
         witness = w:finish() })lua"),
      objectSnippet(
          "lidar_crop_map",
          "The same crop, transformed into the map frame when a transform is available, then the highest point.",
          {{"cloud", "kPointCloud"}},
          R"lua(local half, zmin, zmax = 10, -2, 3
local cropped = cloud:crop_box{min = {-half, -half, zmin}, max = {half, half, zmax}}
local tf, why = pj.tf.lookup("map", cloud.frame_id)
local report
if tf then
  cropped = cropped:transform(tf)
  report = string.format("%d of %d points kept, transformed to map", cropped:count(), cloud:count())
else
  report = "no tf: " .. why
end
local top = cropped:extreme_point("z", "max")
if top == nil then
  return { cropped = cropped, max_z = pj.unavailable("no points in the box"), report = report,
           witness = pj.scene.new{frame_id = cropped.frame_id, id = "highest"}:finish() }
end
local w = pj.scene.new{frame_id = cropped.frame_id, id = "highest"}
  :sphere{center = {top.x, top.y, top.z}, diameter = 0.3, color = {255, 40, 40, 255}}
return { cropped = cropped, max_z = top.z, report = report, witness = w:finish() })lua"),
      objectSnippet(
          "witness_of_crop",
          "Reads the cropped cloud of a lidar_crop recipe and places a sphere on its highest point (in the map frame "
          "when a transform is available).",
          {{"cloud", "kPointCloud"}},
          R"lua(local tf, why = pj.tf.lookup("map", cloud.frame_id)
local report
if tf then
  cloud = cloud:transform(tf)
  report = "cropped cloud transformed to map"
else
  report = "using input frame: " .. why
end
local witness = pj.scene.new{frame_id = cloud.frame_id, id = "crop_highest"}
local top = cloud:extreme_point("z", "max")
if top == nil then
  return { witness = witness:finish(), max_z = pj.unavailable("no points in the crop"), report = "empty crop" }
end
witness:sphere{center = {top.x, top.y, top.z}, diameter = 0.4, color = {40, 220, 255, 255}}
return { witness = witness:finish(), max_z = top.z,
         report = string.format("%d cropped points; %s", cloud:count(), report) })lua"),
      objectSnippet(
          "cam_threshold", "Marks the bright pixels of an image (above 128) as an overlay for that image.",
          {{"image", "kImage"}},
          R"lua(local threshold = 128
local decoded, err = image:decode()
if decoded == nil then
  return { overlay = pj.unavailable(err) }
end
local gray = decoded.channels > 1 and decoded:to_gray() or decoded
return { overlay = gray:threshold(">", threshold) })lua"),
      objectSnippet(
          "cam_annotations",
          "Draws a rectangle and a label with the highest z of a lidar crop on the front camera image.",
          {{"cloud", "kPointCloud"}},
          R"lua(local image_topic = "/cam_front/image_rect_compressed"
local half, zmin, zmax = 10, -2, 3
local cropped = cloud:crop_box{min = {-half, -half, zmin}, max = {half, half, zmax}}
local highest = cropped:extreme_point("z", "max")
local a = pj.annotations.new{image_topic = image_topic}
a:points{points = {{100, 100}, {500, 100}, {500, 300}, {100, 300}}, type = "line_loop", thickness = 3,
         outline_color = {255, 80, 20, 255}}
a:text{position = {110, 120}, text = highest and string.format("max_z %.2f", highest.z) or "no finite points",
       font_size = 24}
return { overlay = a:finish() })lua"),
      objectSnippet(
          "depth_cloud", "Turns a depth image and its camera calibration into a point cloud.",
          {{"depth", "kDepthImage"}, {"camera_info", "kCameraInfo"}},
          R"lua(local step, max_depth = 1, 5.0
return depth:to_point_cloud(camera_info, {step = step, max_depth = max_depth}))lua"),
  };
}

// Default snippets ported from PJ3's default.snippets.xml, then the object ones.
std::vector<Snippet> defaultSnippets() {
  std::vector<Snippet> snippets = {
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
  for (auto& snippet : objectDefaultSnippets()) {
    snippets.push_back(std::move(snippet));
  }
  return snippets;
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
    result.push_back(snippetFromJson(item));
  }
  return result;
}

// The persisted library, or the built-in defaults when none can be read yet —
// a missing, unreadable, or corrupt file all fall back to the defaults (so a
// transient read failure never silently presents an empty library). A persisted library
// written before the object functions existed gets the ones it lacks appended by name.
std::vector<Snippet> loadSnippetsFromDisk() {
  if (auto loaded = loadSnippetsFromPath(snippetLibraryPath())) {
    for (auto& builtin : objectDefaultSnippets()) {
      const bool present =
          std::any_of(loaded->begin(), loaded->end(), [&](const Snippet& s) { return s.name == builtin.name; });
      if (!present) {
        loaded->push_back(std::move(builtin));
      }
    }
    return std::move(*loaded);
  }
  return defaultSnippets();
}

// `s` without leading/trailing spaces and tabs.
inline std::string trimBlanks(const std::string& s) {
  const std::size_t b = s.find_first_not_of(" \t");
  const std::size_t e = s.find_last_not_of(" \t");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
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
    std::string name = trimBlanks(field.substr(start, end - start));
    if (!name.empty()) {
      names.push_back(std::move(name));
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
    std::string src = "# pj-script: python\n";
    if (!global_code.empty()) {
      src += global_code + "\n\n";
    }
    src += "def _pj_fn(" + params + "):\n";
    src += derived_recipes::indentPython(body.empty() ? "return value" : body);
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

// One declared output of an on-demand recipe: the name the script returns it under and its
// type ("number", "string", or a builtin object type such as "kPointCloud").
struct OnDemandOutput {
  std::string name;
  std::string type;
  bool operator==(const OnDemandOutput&) const = default;
};

using derived_recipes::isObjectOutputType;
using derived_recipes::objectTypeLabel;
using derived_recipes::sceneKindForOutputType;

enum class RecipeKind { kTransform, kOnDemand };

// The type of an input that is not a scalar when the host cannot enumerate object topics.
constexpr const char* kUnknownInputType = "unknown";

// The engine is never picked by the user, and outputs never decide it: a recipe is on-demand
// (evaluated where a consumer asks) when it reads an object, otherwise it is the per-sample
// transform. An input type that is not an object type (a number, a string, an unresolved or
// ambiguous name) never forces on-demand, except `kUnknownInputType`: a host without catalog snapshot v2
// cannot say what a non-scalar input is, so it counts as an object. `hint` is a saved on-demand state: its script is an
// on-demand chunk whatever it reads.
RecipeKind deduceEngine(const std::vector<std::string>& input_types, bool hint) {
  const bool object = hint || std::any_of(input_types.begin(), input_types.end(), [](const std::string& type) {
                        return type == kUnknownInputType || isObjectOutputType(type);
                      });
  return object ? RecipeKind::kOnDemand : RecipeKind::kTransform;
}

// Make `request` the ephemeral recipe `id` (never listed, saved or undoable). `instant_ns` pins the
// evaluation instant; none follows the cursor.
void makeEphemeral(
    PJ::sdk::DataProcessorRequest& request, std::string_view id, std::optional<std::int64_t> instant_ns) {
  request.id = std::string(id);
  request.flags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL;
  request.instant_ns = instant_ns;
}

// What a request installs, as a string: two requests with the same signature are the same recipe.
std::string requestSignature(const PJ::sdk::DataProcessorRequest& request) {
  nlohmann::json signature = {
      request.id,          request.flags,
      request.inputs,      request.script,
      request.params_json, request.instant_ns ? nlohmann::json(*request.instant_ns) : nlohmann::json(nullptr)};
  for (const auto& output : request.outputs) {
    signature.push_back({output.name, output.type});
  }
  return signature.dump();
}

// The params field is a JSON object (empty means "{}"). nullopt when it is not one.
std::optional<nlohmann::json> parseParamsObject(const std::string& text) {
  const std::size_t b = text.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return nlohmann::json::object();
  }
  auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::nullopt;
  }
  return parsed;
}

// The key an on-demand recipe's params_json reserves for the editor's own state.
constexpr const char* kEditorParamsKey = "__editor";

// Why the params field cannot be used, or empty when it can.
std::string paramsError(const std::string& text) {
  const auto parsed = parseParamsObject(text);
  if (!parsed) {
    return "Params must be a JSON object";
  }
  if (parsed->contains(kEditorParamsKey)) {
    return std::string("Params key \"") + kEditorParamsKey + "\" is reserved by the editor";
  }
  return {};
}

// ---------------------------------------------------------------------------
// Readable summary of an on-demand evaluation report
// ---------------------------------------------------------------------------

std::string groupDigits(std::uint64_t value) {
  std::string digits = std::to_string(value);
  for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
    digits.insert(static_cast<std::size_t>(at), " ");
  }
  return digits;
}

// A coordinate with at most 6 significant digits and no trailing zeros.
std::string shortNumber(const nlohmann::json& value) {
  if (!value.is_number()) {
    return "?";
  }
  std::ostringstream out;
  out << std::setprecision(6) << value.get<double>();
  return out.str();
}

// What one object output holds, from the "summary" the host puts in a report.
std::string summarizeObjectOutput(const nlohmann::json& summary) {
  std::string text = objectTypeLabel(summary.value("type", std::string{"object"}));
  const auto count = [&](const char* key, const char* noun) {
    if (summary.contains(key) && summary[key].is_number_unsigned()) {
      text += ", " + groupDigits(summary[key].get<std::uint64_t>()) + " " + noun;
    }
  };
  count("points", "points");
  count("entities", "entities");
  count("deletions", "deletions");
  count("circles", "circles");
  count("texts", "texts");
  count("transforms", "transforms");
  return text;
}

// A number as the readout shows it; anything else is `fallback` followed by the host's reason, when it
// gives one: "unavailable (no points in the box)".
std::string formatValue(const nlohmann::json& entry, const std::string& fallback) {
  if (entry.contains("value") && entry["value"].is_number()) {
    return shortNumber(entry["value"]);
  }
  std::string text = fallback;
  if (entry.contains("reason") && entry["reason"].is_string() && !entry["reason"].get<std::string>().empty()) {
    text += " (" + entry["reason"].get<std::string>() + ")";
  }
  return text;
}

// What a trial evaluation (INFER_OUTPUTS, no declared outputs) learned, parsed ONCE from the report the
// host returns from poll_evaluation: the outputs the script returned, whether one of them was
// unavailable at this instant (its type is not known yet), whether the instant had a sample at all, and
// the host's error when the script failed. `summary` is one line per output of the first bundle
// (anything that is not a report is shown as it is) and `readout` the "name: value" lines of its number
// outputs; both are derived here so nothing re-reads the JSON afterwards.
struct TrialReport {
  std::vector<OnDemandOutput> outputs;
  bool has_unknown = false;
  bool has_sample = false;
  std::string error;
  std::string summary;
  std::string readout;
};

TrialReport parseTrialReport(const std::string& report) {
  TrialReport trial;
  const auto parsed = nlohmann::json::parse(report, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    trial.error = report;
    trial.summary = report;
    return trial;
  }
  if (parsed.contains("error") && parsed["error"].is_string()) {
    trial.error = parsed["error"].get<std::string>();
    trial.summary = trial.error;
    return trial;
  }
  // A run the host stopped on an error says so in `coverage.error`; the bundles it left behind do not.
  const auto coverage = parsed.find("coverage");
  if (coverage != parsed.end() && coverage->is_object() && coverage->contains("error") &&
      (*coverage)["error"].is_string()) {
    trial.error = (*coverage)["error"].get<std::string>();
    trial.summary = trial.error;
    return trial;
  }
  const auto bundles = parsed.find("bundles");
  trial.has_sample = bundles != parsed.end() && bundles->is_array() && !bundles->empty();
  const nlohmann::json* produced = nullptr;  // the "outputs" object of the first bundle
  if (trial.has_sample && (*bundles)[0].is_object() && (*bundles)[0].contains("outputs") &&
      (*bundles)[0]["outputs"].is_object()) {
    produced = &(*bundles)[0]["outputs"];
  }
  if (produced == nullptr) {
    trial.summary = "no result at the cursor";
  } else {
    for (const auto& [name, entry] : produced->items()) {
      const std::string status = entry.value("status", std::string{});
      std::string line = name + ": ";
      if (entry.contains("summary") && entry["summary"].is_object()) {
        line += (status == "empty" ? "empty " : "") + summarizeObjectOutput(entry["summary"]);
      } else if (entry.contains("value") && !entry["value"].is_number()) {
        line += entry["value"].is_string() ? entry["value"].get<std::string>() : entry["value"].dump();
      } else {
        line += formatValue(entry, status.empty() ? std::string("no value") : status);
      }
      trial.summary += (trial.summary.empty() ? "" : "\n") + line;
      if (status == "error" && trial.error.empty()) {
        trial.error = entry.value("reason", name + ": error");
      }
    }
    if (trial.summary.empty()) {
      trial.summary = "no outputs";
    }
    if (!trial.error.empty()) {
      return trial;
    }
  }
  const auto outputs = parsed.find("outputs");
  if (outputs != parsed.end() && outputs->is_array()) {
    for (const auto& output : *outputs) {
      if (output.is_object() && output.contains("name") && output["name"].is_string()) {
        const std::string type = output.value("type", std::string{"unknown"});
        trial.has_unknown = trial.has_unknown || type == "unknown";
        trial.outputs.push_back({output["name"].get<std::string>(), type});
      }
    }
  }
  if (produced != nullptr) {
    for (const auto& output : trial.outputs) {
      if (output.type == "number" && produced->contains(output.name)) {
        trial.readout += (trial.readout.empty() ? "" : "\n") + output.name + ": " +
                         formatValue((*produced)[output.name], "unavailable");
      }
    }
  }
  return trial;
}

// How many values the body returns per sample, read from its `return` statements: the commas at the
// top level of a return's expression list, the most of any return (an early `return nil` does not
// count). 1 when no return has a value. Strings, comments and brackets are skipped.
std::size_t returnArity(const std::string& body, const std::string& language) {
  const bool python = language == "python";
  const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
  std::size_t best = 0;
  std::size_t i = 0;
  const std::size_t n = body.size();
  const auto skip_string = [&](std::size_t at) {  // `at` is on the opening quote; returns the index after the string
    const char quote = body[at];
    if (python && body.compare(at, 3, std::string(3, quote)) == 0) {
      const std::size_t close = body.find(std::string(3, quote), at + 3);
      return close == std::string::npos ? n : close + 3;
    }
    std::size_t k = at + 1;
    while (k < n && body[k] != quote && body[k] != '\n') {
      k += body[k] == '\\' ? 2 : 1;
    }
    return std::min(k + 1, n);
  };
  const auto skip_lua_long =
      [&](std::size_t at) {  // `at` is on "[": the index after a [[...]] / [=[...]=] block, or at
        std::size_t k = at + 1;
        std::size_t level = 0;
        while (k < n && body[k] == '=') {
          ++level, ++k;
        }
        if (k >= n || body[k] != '[') {
          return at;
        }
        const std::size_t close = body.find("]" + std::string(level, '=') + "]", k + 1);
        return close == std::string::npos ? n : close + level + 2;
      };
  while (i < n) {
    const char c = body[i];
    if (c == '"' || c == '\'') {
      i = skip_string(i);
    } else if (python && c == '#') {
      while (i < n && body[i] != '\n') {
        ++i;
      }
    } else if (!python && c == '-' && i + 1 < n && body[i + 1] == '-') {
      const std::size_t after = i + 2 < n && body[i + 2] == '[' ? skip_lua_long(i + 2) : i + 2;
      if (after != i + 2) {
        i = after;
      } else {
        while (i < n && body[i] != '\n') {
          ++i;
        }
      }
    } else if (!python && c == '[' && skip_lua_long(i) != i) {
      i = skip_lua_long(i);
    } else if (is_word(c)) {
      std::size_t end = i;
      while (end < n && is_word(body[end])) {
        ++end;
      }
      if (body.compare(i, end - i, "return") != 0) {
        i = end;
        continue;
      }
      // Parse the expression list after `return`.
      std::size_t k = end;
      int depth = 0;
      std::size_t commas = 0;
      bool any = false;
      for (; k < n; ++k) {
        const char d = body[k];
        if (d == '"' || d == '\'') {
          any = true;
          k = skip_string(k) - 1;
          continue;
        }
        if (d == '(' || d == '[' || d == '{') {
          any = true;
          ++depth;
        } else if (d == ')' || d == ']' || d == '}') {
          --depth;
        } else if (depth == 0 && d == ';') {
          break;
        } else if (depth == 0 && d == '\n') {
          // A Lua list may continue on the next line after a trailing comma.
          std::size_t back = k;
          while (back > end && std::isspace(static_cast<unsigned char>(body[back - 1])) != 0) {
            --back;
          }
          if (python || back == end || body[back - 1] != ',') {
            break;
          }
        } else if (depth == 0 && d == ',') {
          ++commas;
        } else if (depth == 0 && !python && is_word(d) && (k == 0 || !is_word(body[k - 1]))) {
          std::size_t word_end = k;
          while (word_end < n && is_word(body[word_end])) {
            ++word_end;
          }
          const std::string word = body.substr(k, word_end - k);
          if (word == "end" || word == "else" || word == "elseif" || word == "until") {
            break;
          }
          any = true;
          k = word_end - 1;
        } else if (depth == 0 && python && d == '#') {
          break;
        } else if (std::isspace(static_cast<unsigned char>(d)) == 0) {
          any = true;
        }
      }
      if (any) {
        best = std::max(best, commas + 1);
      }
      i = k;
    } else {
      ++i;
    }
  }
  return std::clamp<std::size_t>(best, 1, 8);
}

// "Series" for a per-sample function, "2D" when every input of an object function is an image-like type, else "3D".
std::string snippetKindLabel(const Snippet& snippet) {
  if (snippet.kind != "object") {
    return "Series";
  }
  const bool all_2d =
      !snippet.inputs.empty() && std::all_of(snippet.inputs.begin(), snippet.inputs.end(), [](const auto& in) {
        return derived_recipes::sceneKindForOutputType(in.type) == "2d";
      });
  return all_2d ? "2D" : "3D";
}

// "cloud (point cloud), camera_info (camera info)"
std::string snippetInputsText(const std::vector<SnippetInput>& inputs) {
  std::string text;
  for (const auto& input : inputs) {
    text += (text.empty() ? "" : ", ") + input.var + " (" + objectTypeLabel(input.type) + ")";
  }
  return text;
}

// ---------------------------------------------------------------------------
// Preview chart
// ---------------------------------------------------------------------------

// (absolute ns, value) samples of one preview curve.
using RawSamples = std::vector<std::pair<std::int64_t, double>>;

// TODO(theme): These data-series colors are a deliberate color-as-data exception to the theme palette: a
// curve's identity is carried by its color here. Use renderer-selected colors once the chart protocol
// exposes them. #RRGGBB, cycled by series index so parallel outputs stay visually distinct.
constexpr const char* kSeriesColors[] = {"#ff8800", "#0088ff", "#22aa22", "#cc2222", "#9933cc", "#00a0a0"};
constexpr std::size_t kSeriesColorCount = sizeof(kSeriesColors) / sizeof(kSeriesColors[0]);

// The first timestamp of the earliest curve (0 when every curve is empty): the common t0 that aligns them.
std::int64_t earliestTimestamp(const std::vector<RawSamples>& curves) {
  std::int64_t t0 = 0;
  bool have = false;
  for (const auto& raw : curves) {
    if (!raw.empty() && (!have || raw.front().first < t0)) {
      t0 = raw.front().first;
      have = true;
    }
  }
  return t0;
}

std::vector<PJ::ChartPoint> toChartPoints(const RawSamples& raw, std::int64_t t0) {
  std::vector<PJ::ChartPoint> points;
  points.reserve(raw.size());
  for (const auto& [ts, v] : raw) {
    points.push_back({static_cast<double>(ts - t0) / 1e9, v});
  }
  return points;
}

// One solid curve per non-empty entry of `raw`, labelled by `labels[k]` and colored by its index k, in
// seconds from `t0`.
std::vector<PJ::ChartSeries> toChartSeries(
    const std::vector<RawSamples>& raw, const std::vector<std::string>& labels, std::int64_t t0) {
  std::vector<PJ::ChartSeries> series;
  for (std::size_t k = 0; k < raw.size(); ++k) {
    if (!raw[k].empty()) {
      series.push_back({labels[k], toChartPoints(raw[k], t0), kSeriesColors[k % kSeriesColorCount], /*dashed=*/false});
    }
  }
  return series;
}

// ---------------------------------------------------------------------------
// TransformEditorDialog
// ---------------------------------------------------------------------------

class TransformEditorDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

  static constexpr const char* kNeedInputProblem = "Add an input (drag & drop a series or an object topic)";
  static constexpr const char* kNeedBodyProblem = "Write your function body";
  static constexpr const char* kNeedNewHostProblem = "Object inputs require a PlotJuggler host with SDK 0.36 or newer";

 public:
  std::string manifest() const override {
    return kTransformEditorManifest;
  }
  std::string ui_content() const override {
    return kTransformEditorDialogUi;
  }

  // The header label of the function pane cannot shrink (the host band gives it its full text width), so a long
  // signature is cut here to leave the Lua/Python radios and the library buttons their room.
  static std::string elideSignature(const std::string& signature) {
    constexpr std::size_t kMaxChars = 28;
    if (signature.size() <= kMaxChars) {
      return signature;
    }
    return signature.substr(0, kMaxChars - 3) + "...";
  }

  PJ::WidgetData buildWidgetData() {
    PJ::WidgetData wd;

    // One-shot after loadConfig (Modify): force the editor onto the tab the series
    // was created from. Only once, so the user can freely switch tabs afterwards.
    if (pending_tab_restore_) {
      wd.setTabIndex("tabWidget", current_tab_);
      pending_tab_restore_ = false;
    }

    // Single function tab: one table of inputs (drop target). Col 0 is the radio marking which
    // row provides `value` (series only); then the input, the Var the script reads it by, and the
    // type of the input in words.
    const bool on_demand = isOnDemand();
    const std::vector<std::string>& vars = variableNames();
    wd.setDropTarget("tableSources");
    wd.setTableHeaders("tableSources", {"", "Input", "Var", "Type"});
    std::vector<std::vector<std::string>> rows;
    rows.reserve(sources_.size());
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      rows.push_back({"", sources_[i], vars[i], objectTypeLabel(inputTypeOf(sources_[i]))});
    }
    wd.setTableRows("tableSources", rows);
    wd.setListItemsDeletable("tableSources", true);
    if (!on_demand) {
      wd.setTableRadioColumn("tableSources", 0, primaryIndex());
    }

    // Function signature reflects the inputs: `time, value, v1..vN` for series (the radio picks
    // `value`), the Var of every input for objects.
    std::string signature;
    if (on_demand) {
      signature = "function( ";
      for (std::size_t i = 0; i < vars.size(); ++i) {
        signature += (i == 0 ? "" : ", ") + vars[i];
      }
      signature += vars.empty() ? ")" : " )";
    } else {
      signature = "function( time, value";
      for (std::size_t i = 0; i + 1 < sources_.size(); ++i) {
        signature += ", v" + std::to_string(i + 1);
      }
      signature += " )";
    }
    wd.setText("functionTitle", elideSignature(signature));

    const char* single_lang = (language_ == "python") ? "python" : "lua";
    wd.setCodeContent("globalVarsText", global_code_).setCodeLanguage("globalVarsText", single_lang);
    wd.setCodeContent("functionText", function_body_).setCodeLanguage("functionText", single_lang);
    // Reflect the active language on the radios (so loading a library snippet flips
    // them, not just the user clicking). Pushed every tick; matches language_.
    wd.setChecked("luaButton", language_ != "python");
    wd.setChecked("pythonButton", language_ == "python");

    // The advanced disclosure (params and pinning, recipes evaluated at the cursor only) and the scene
    // button. Inputs are added by drag and drop only.
    wd.setVisible("buttonAdvanced", on_demand);
    wd.setButtonText("buttonAdvanced", advanced_open_ ? "Advanced v" : "Advanced >");
    wd.setVisible("advancedPane", on_demand && advanced_open_);
    wd.setText("paramsLineEdit", params_text_);
    const std::string& params_error = derived().params_error;
    wd.setFieldValid("paramsLineEdit", params_error.empty(), params_error);
    wd.setChecked("pinCurrentTimeCheck", pin_current_);
    wd.setVisible("buttonShowScene", !scene_button_.empty());
    if (!scene_button_.empty()) {
      wd.setButtonText("buttonShowScene", scene_button_);
    }

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
    // "Create..." name prompt: the name field prefilled, what will be created, and a note when the
    // last attempt was refused or the name would replace a recipe.
    if (emit_create_dialog_) {
      wd.setText("createRecipeName", create_name_);
      wd.setLabel("createRecipeSummary", createSummary());
      wd.setLabel("createRecipeNote", create_note_);
      wd.requestSubDialog(kCreateRecipeUi);
      emit_create_dialog_ = false;
    }
    if (library_open_) {
      const std::vector<std::string> names = filteredSnippetNames();
      wd.setTableHeaders("tableFunctions", {"Function", "Kind", "Language"});
      std::vector<std::vector<std::string>> lib_rows;
      lib_rows.reserve(names.size());
      for (const auto& n : names) {
        auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == n; });
        const std::string lang = (it != snippets_.end() && it->language == "python") ? "Python" : "Lua";
        lib_rows.push_back({n, it != snippets_.end() ? snippetKindLabel(*it) : "Series", lang});
      }
      wd.setTableRows("tableFunctions", lib_rows);
      wd.setLabel("previewInfoLabel", snippetInfoText(library_selected_));
      wd.setCodeContent("previewPlainText", combinedSnippetText(library_selected_))
          .setCodeLanguage("previewPlainText", "lua");
    }
    // Import / Export library buttons: the host drives the native file choosers.
    // Import opens an "open" dialog and Export a "save as"; both report back via
    // onFileSelected, routed by widget name. Complements the library browser above.
    wd.setFilePicker("buttonLoadFunctions", "", "Snippet library (*.json)", "Import snippet library");
    wd.setSaveFilePicker("buttonSaveFunctions", "", "Snippet library (*.json)", "Export snippet library", "json");

    // Over the chart: what is missing, then the host's message (the real compile/run error), then the
    // readout of a recipe evaluated at the cursor when it has no series to plot. A one-shot Import/Export
    // line goes first and clears on the next build.
    std::string overlay;
    for (const std::string& problem : formProblems()) {
      overlay += problem + "\n";
    }
    if (!validation_error_.empty()) {
      overlay += validation_error_ + "\n";
    }
    if (overlay.empty() && on_demand && preview_series_.empty()) {
      overlay = readout_;
    }
    if (!io_status_.empty()) {
      overlay = overlay.empty() ? io_status_ : io_status_ + "\n" + overlay;
      io_status_.clear();
    }
    // Panes: the plot for numbers (and for anything the overlay has to say), the embedded scene for objects.
    const bool scene_embedded = !scene_embed_kind_.empty();
    wd.setVisible("framePlotPreview", !scene_embedded || preview_has_numbers_ || !overlay.empty());
    wd.setVisible("frameScenePreview", scene_embedded);
    if (scene_embedded) {
      wd.setSceneView("frameScenePreview", scene_embed_kind_);
      wd.setSceneTopics("frameScenePreview", scene_embed_topics_);
    } else {
      wd.clearSceneView("frameScenePreview");  // full state every tick: the host diffs each entry
    }
    if (!overlay.empty()) {
      wd.clearChart("framePlotPreview");
      wd.setChartPlaceholder("framePlotPreview", overlay);
    } else {
      wd.setChartPlaceholder("framePlotPreview", "");
      wd.setChartSeries("framePlotPreview", preview_series_);
      wd.setChartAutoZoom("framePlotPreview", true);
    }

    // The one-line status: the result of the last trial, else why Create is disabled.
    const std::string reason = canCreateReason();
    std::string status = status_text_;
    if (!needs_note_.empty()) {
      status = needs_note_;
    } else if (status.empty() || (!reason.empty() && !validation_error_.empty())) {
      status = reason;
    }
    wd.setLabel("statusLabel", oneLine(status));

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
      batch_term += std::string(kNeedBodyProblem) + "\n";
    }
    if (!batch_validation_error_.empty()) {
      batch_term += batch_validation_error_ + "\n";
    }
    wd.setChartPlaceholder("framePlotPreviewBatch", batch_term);

    // Each tab owns its Create action and validation gate. The reason a disabled Create is disabled goes
    // on the status line and, where the host shows it, on the button.
    wd.setEnabled("pushButtonCreate", reason.empty());
    wd.setButtonText("pushButtonCreate", edit_mode_ ? "Modify" : "Create…");
    wd.setFieldValid("pushButtonCreate", reason.empty(), reason);
    // Missing affix and empty inputs gate via the disabled button, not the overlay.
    wd.setEnabled("pushButtonCreateBatch", batch_term.empty() && !batch_sources_.empty() && !batch_suffix_.empty());
    wd.setButtonText("pushButtonCreateBatch", edit_mode_ ? "Modify Time Series" : "Create New Time Series");
    // Lock the identity while editing so a rename can't fork a new series (PJ3
    // parity). Batch: the sources + prefix/suffix that form the name. setEnabled(false) is
    // cosmetic for drops — see onItemsDropped.
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
    // Name typed in the "Create..." prompt (harvested on OK).
    if (name == "createRecipeName") {
      pending_create_name_ = std::string(text);
      return true;
    }
    if (name == "paramsLineEdit") {
      params_text_ = std::string(text);
      formChanged();
      return true;
    }
    return false;
  }

  bool onCodeChanged(std::string_view name, std::string_view text) override {
    if (name == "globalVarsText") {
      global_code_ = std::string(text);
      formChanged();
      return true;
    }
    if (name == "functionText") {
      if (text == function_body_) {
        return true;  // the host echoes the code it was just given; that is not an edit
      }
      function_body_ = std::string(text);
      auto_body_.clear();  // the user's own body: never rewritten by the editor
      body_edit_ = true;   // an empty body is the user's, not the template
      formChanged();
      body_edit_ = false;
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
      // Modify keeps the name (locked) and asks nothing; Create asks for the name first.
      if (edit_mode_) {
        pending_create_ = PendingCreate::Single;
      } else if (canCreateReason().empty()) {
        openCreatePrompt();
      }
      return true;
    }
    if (name == "buttonAdvanced") {
      advanced_open_ = !advanced_open_;
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
      var_overrides_.clear();
      primary_index_ = -1;
      inputsChanged();
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
      create_stage_ = CreateStage::None;
      save_stage_ = SaveStage::NamePrompt;
      emit_save_name_dialog_ = true;
      return true;
    }
    // A modal sub-dialog was accepted (OK). Drives the save state machine.
    if (name == "subDialogAccepted") {
      if (create_stage_ == CreateStage::Prompt) {
        acceptCreatePrompt();
      } else if (save_stage_ == SaveStage::NamePrompt) {
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
    if (name == "buttonShowScene") {
      show_scene_requested_ = true;
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
    var_overrides_.erase(var_overrides_.begin() + index);
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
    inputsChanged();
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
    if (show_scene_requested_) {
      show_scene_requested_ = false;
      if (on_show_scene_) {
        on_show_scene_();
      }
    }
    // Rebuild immediately after an editor change. Browser builds sample an
    // otherwise-live source at 1 Hz instead of replaying the complete Luau
    // transform on every 20 Hz panel tick; native builds retain their existing
    // every-tick live preview. Streaming output still advances in the host on
    // every source commit—this cadence only rebuilds the preview snapshot.
    ++preview_refresh_ticks_;
    if (preview_dirty_ || preview_refresh_ticks_ >= kPreviewRefreshTickInterval) {
      preview_dirty_ = false;
      preview_refresh_ticks_ = 0;
      if (on_refresh_preview_) {
        on_refresh_preview_();
      }
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
      formChanged();
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
    if (name == "pinCurrentTimeCheck") {
      pin_current_ = checked;
      return true;
    }
    // Single-tab script language (Lua / Python); the preview re-runs so the host's verdict on the
    // chosen language surfaces.
    if (name == "luaButton" && checked) {
      language_ = "luau";
      formChanged();
      return true;
    }
    if (name == "pythonButton" && checked) {
      language_ = "python";
      formChanged();
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
      for (const auto& item : items) {
        addSource(item);
      }
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
    // The on-demand form fields ride along only for that kind, so a transform's saved state
    // keeps exactly today's shape.
    if (isOnDemand()) {
      cfg["kind"] = "on_demand";
      cfg["vars"] = variableNames();
      cfg["params_text"] = params_text_;
      cfg["pin_current_time"] = pin_current_;
    }
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
    if (cfg.is_discarded() || !cfg.is_object()) {
      return false;
    }
    // An on-demand recipe stores the user's params object plus "__editor", the editor's own
    // state; a transform (and any older state) stores the editor state as the whole document.
    std::string user_params_text;
    if (cfg.contains(kEditorParamsKey) && cfg[kEditorParamsKey].is_object()) {
      nlohmann::json user_params = cfg;
      user_params.erase(kEditorParamsKey);
      user_params_text = user_params.empty() ? std::string{} : user_params.dump();
      nlohmann::json state = cfg[kEditorParamsKey];
      cfg = std::move(state);
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
    auto_body_.clear();
    language_ = cfg.value("language", std::string{"luau"});
    loadKindState(cfg, user_params_text);
    current_tab_ = 0;             // open on the Single tab
    pending_tab_restore_ = true;  // one-shot: push the tab to the UI
    sources_.clear();
    var_overrides_.clear();
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
    // The Vars an on-demand state saved (the resolved names).
    // Only a name that is not the default of its row is kept as the user's own.
    var_overrides_.assign(sources_.size(), std::string{});
    if (cfg.contains("vars") && cfg["vars"].is_array() && cfg["vars"].size() == sources_.size()) {
      const std::vector<std::string> defaults = onDemandVariableNames(var_overrides_);
      for (std::size_t i = 0; i < sources_.size(); ++i) {
        if (cfg["vars"][i].is_string() && cfg["vars"][i].get<std::string>() != defaults[i]) {
          var_overrides_[i] = cfg["vars"][i].get<std::string>();
        }
      }
    }
    // Loading a populated config = editing an existing series → MODIFY mode: the
    // button reads "Modify" and the name is locked so the user can't accidentally
    // fork a new series (PJ3 parity: editExistingPlot disables nameLineEdit).
    edit_mode_ = !output_name_.empty();
    ++form_revision_;  // the sources, Vars, body and params were all replaced
    return true;
  }

  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }
  void setInputTypeResolver(std::function<std::string(const std::string&)> cb) {
    input_type_of_ = std::move(cb);
  }
  void setOnShowScene(std::function<void()> cb) {
    on_show_scene_ = std::move(cb);
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
  // handled by the caller (the save state machine), mirroring PJ3. A function that reads objects
  // is saved as an object function with the Var and type of each object input.
  Snippet currentAsSnippet(const std::string& snippet_name) const {
    Snippet sn;
    sn.name = snippet_name;
    sn.global_code = global_code_;
    sn.function_body = function_body_;
    sn.language = language_;
    if (isOnDemand()) {
      sn.kind = "object";
      const std::vector<std::string> vars = variableNames();
      for (std::size_t i = 0; i < sources_.size(); ++i) {
        if (const std::string type = inputTypeOf(sources_[i]); isObjectOutputType(type)) {
          sn.inputs.push_back({vars[i], type});
        }
      }
    }
    return sn;
  }

  void doSaveSnippet(const std::string& snippet_name) {
    Snippet sn = currentAsSnippet(snippet_name);
    auto it =
        std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == snippet_name; });
    if (it != snippets_.end()) {
      sn.description = it->description;
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

  // The description and the inputs the first selected function needs, for the library's preview.
  std::string snippetInfoText(const std::vector<std::string>& names) const {
    for (const auto& n : names) {
      auto it = std::find_if(snippets_.begin(), snippets_.end(), [&](const Snippet& s) { return s.name == n; });
      if (it == snippets_.end()) {
        continue;
      }
      std::string text = it->description;
      if (!it->inputs.empty()) {
        text += (text.empty() ? "" : "\n") + std::string("Inputs: ") + snippetInputsText(it->inputs);
      }
      return text;
    }
    return {};
  }

  // Load the selected library function(s) into the Single-tab editor (combined
  // globals + body), then re-validate. Mirrors PJ3's "Use" / double-click. For a function that
  // reads objects, each input it declares renames the Var of the first input of that type
  // not yet bound; an input with no match is named on the status line.
  void loadSnippetsIntoEditor(const std::vector<std::string>& names) {
    if (names.empty()) {
      return;
    }
    std::string globals;
    std::string body;
    bool language_set = false;
    std::vector<SnippetInput> wanted;
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
      wanted.insert(wanted.end(), it->inputs.begin(), it->inputs.end());
    }
    global_code_ = globals;
    function_body_ = body;
    auto_body_.clear();
    if (!edit_mode_) {
      output_name_ = names.front();  // seed with the first; the user can rename
    }
    formChanged();
    bindSnippetInputs(wanted);  // after formChanged, which drops the "needs:" remark of the previous load
  }

  // Name the Var of the first unbound input of each wanted type after the function's variable.
  void bindSnippetInputs(const std::vector<SnippetInput>& wanted) {
    needs_note_.clear();
    std::vector<bool> bound(sources_.size(), false);
    std::string missing;
    for (const auto& input : wanted) {
      bool found = false;
      for (std::size_t i = 0; i < sources_.size() && !found; ++i) {
        if (!bound[i] && inputTypeOf(sources_[i]) == input.type) {
          bound[i] = true;
          var_overrides_[i] = input.var;
          found = true;
        }
      }
      if (!found) {
        missing += (missing.empty() ? "" : ", ") + input.var + " (" + objectTypeLabel(input.type) + ")";
      }
    }
    if (!missing.empty()) {
      needs_note_ = "needs: " + missing;
    }
    ++form_revision_;  // the Vars changed
  }

  /// Replaces the plotted series; an identical set (the usual case while nothing changes) is kept as it is.
  void setPreviewSeries(std::vector<PJ::ChartSeries> series) {
    if (!sameSeries(series, preview_series_)) {
      preview_series_ = std::move(series);
    }
  }
  /// The readout shown over the chart of a recipe evaluated at the cursor while there is no series to plot.
  void setReadout(std::string text) {
    readout_ = std::move(text);
  }
  /// The one-line result of the last trial ("max_z: 1.83, cropped: point cloud, 23 144 points", joined by a middle
  /// dot).
  void setStatus(std::string text) {
    status_text_ = std::move(text);
  }
  /// The object outputs of the preview recipe shown by the scene view embedded next to the chart: its kind
  /// ("3d" | "2d") and the (topic, dataset) pairs it follows. An empty kind removes the view.
  void setEmbeddedScene(std::string kind, std::vector<PJ::SceneTopic> topics) {
    scene_embed_kind_ = std::move(kind);
    scene_embed_topics_ = std::move(topics);
  }
  /// Whether the preview recipe has a number output. With an embedded scene the plot is hidden when it has none.
  void setPreviewHasNumbers(bool has_numbers) {
    preview_has_numbers_ = has_numbers;
  }
  /// The outcome of a trial evaluation, parsed once: the outputs the script returned (an `unknown` type means the
  /// value was unavailable at that instant). Create needs a trial that succeeded without any unknown output.
  void setTrial(TrialReport report) {
    report.error.clear();
    storeTrial(std::move(report));
  }
  /// A trial that failed (the host's message) or found nothing to run on.
  void setTrialFailure(std::string error) {
    TrialReport report;
    report.error = std::move(error);
    storeTrial(std::move(report));
  }
  void clearTrial() {
    storeTrial(std::nullopt);
  }
  const std::vector<OnDemandOutput>& inferredOutputs() const {
    static const std::vector<OnDemandOutput> kNone;
    return trial_ ? trial_->outputs : kNone;
  }
  /// The "name: value" lines of the number outputs of the last successful trial (empty after a failure).
  const std::string& trialReadout() const {
    static const std::string kNone;
    return trial_ ? trial_->readout : kNone;
  }
  /// Changes whenever the inferred outputs do (not on every trial: a rerun that finds the same outputs keeps it).
  std::uint64_t trialSerial() const {
    return trial_serial_;
  }
  /// The last trial succeeded and every output has a type (none was unavailable at that instant).
  bool trialUsable() const {
    return trial_ && !trial_->outputs.empty() && !trial_->has_unknown;
  }
  /// The last trial failed (the host's message, or nothing to run on).
  bool trialFailed() const {
    return trial_ && !trial_->error.empty();
  }

  /// Why Create is disabled, or empty when it is enabled.
  std::string canCreateReason() const {
    if (const auto& problems = formProblems(); !problems.empty()) {
      return problems.front();
    }
    if (!validation_error_.empty()) {
      return validation_error_;
    }
    if (isOnDemand()) {
      if (trialFailed()) {
        return trial_->error;
      }
      if (trial_ && trial_->has_unknown) {
        return "An output is unavailable at this instant, so its type is not known yet: move the cursor to a "
               "frame where it has a value";
      }
      if (!trial_ || trial_->outputs.empty()) {
        return "Waiting for the script to run";
      }
    }
    return {};
  }

  /// One line on what Create will make, for the name prompt: "series `max_z`, point cloud `cropped`;
  /// computed per /lidar_top frame" or "series `speed`; computed per sample".
  std::string createSummary() const {
    if (!isOnDemand()) {
      const std::string name = create_name_.empty() ? std::string("result") : create_name_;
      return "series `" + name + "`; computed per sample";
    }
    std::string text;
    for (const auto& output : inferredOutputs()) {
      const std::string kind = output.type == "number"   ? "series"
                               : output.type == "string" ? "text"
                                                         : objectTypeLabel(output.type);
      text += (text.empty() ? "" : ", ") + kind + " `" + output.name + "`";
    }
    std::string per = "sample";
    for (const std::string& source : sources_) {
      if (isObjectOutputType(inputTypeOf(source))) {
        per = source + " frame";
        break;
      }
    }
    return text + "; computed per " + per;
  }

  /// Why `name` cannot name what is created, or empty. The name may list several outputs separated by commas
  /// for a transform; each one is checked.
  std::string createNameError(const std::string& name) const {
    const std::vector<std::string> names = isOnDemand() ? std::vector<std::string>{name} : splitOutputNames(name);
    if (names.empty() || trimBlanks(name).empty()) {
      return "Give it a name";
    }
    for (const std::string& each : names) {
      if (each.find("__") != std::string::npos) {
        return "A name cannot contain \"__\"";
      }
      if (std::find(sources_.begin(), sources_.end(), each) != sources_.end()) {
        return "'" + each + "' is one of the inputs: give it another name";
      }
    }
    return {};
  }

  /// Names of the editor's own recipes already installed, asked of the host when the Create prompt opens
  /// (a name in this list is a Replace).
  void setOwnRecipeLookup(std::function<bool(const std::string&)> cb) {
    own_recipe_exists_ = std::move(cb);
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
  // --- on-demand form ---
  RecipeKind kind() const {
    return derived().kind;
  }
  bool isOnDemand() const {
    return kind() == RecipeKind::kOnDemand;
  }
  /// Counts the edits of everything the form's derived values (engine, Vars, problems, the request a
  /// toolbox builds from it) are computed from. Equal revisions mean equal derived values.
  std::uint64_t formRevision() const {
    return form_revision_;
  }
  /// The catalog the input types are read from was re-read, so an input may now be of another type.
  void catalogChanged() {
    ++form_revision_;
  }
  /// False on a host without the 0.36 surfaces (object inputs cannot be used then).
  void setOnDemandSupported(bool supported) {
    if (supported != on_demand_supported_) {
      ++form_revision_;
    }
    on_demand_supported_ = supported;
  }
  bool onDemandSupported() const {
    return on_demand_supported_;
  }
  // Whether the host binds a `scene_view` frame of this dialog to a live scene view.
  bool embedsSceneViews() const {
    return hostHas(PJ::DialogHostCapability::kEmbedsSceneViews);
  }
  const std::vector<std::string>& sources() const {
    return sources_;
  }
  /// The variable of every input as the script reads it. Series only: `value`, `v1`, ... as the transform binds
  /// them. With an object input: the Var column, the user's or the default taken from the topic leaf.
  const std::vector<std::string>& variableNames() const {
    return derived().vars;
  }
  /// Why the params field cannot be used, or empty when it can.
  const std::string& paramsProblem() const {
    return derived().params_error;
  }
  /// What stops the form from being run or created, in the order it is shown: no input, no body, an
  /// unsupported host, bad params. The hints over the chart list them all; Create's reason is the first.
  const std::vector<std::string>& formProblems() const {
    return derived().problems;
  }
  /// The Vars of an on-demand recipe for the given overrides ("" = default): a name the user set (or a library
  /// function bound) first, then the defaults in row order.
  std::vector<std::string> onDemandVariableNames(const std::vector<std::string>& overrides) const {
    std::vector<std::string> names(sources_.size());
    std::set<std::string> taken;
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (!overrides[i].empty() && taken.insert(overrides[i]).second) {
        names[i] = overrides[i];
      }
    }
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      if (names[i].empty()) {
        names[i] = derived_recipes::inferredVariableName(sources_[i], taken);
        taken.insert(names[i]);
      }
    }
    return names;
  }
  /// (Var, input) pairs of an on-demand body, in row order.
  std::vector<derived_recipes::InputBinding> variableBindings() const {
    std::vector<derived_recipes::InputBinding> bindings;
    const std::vector<std::string> names = variableNames();
    for (std::size_t i = 0; i < sources_.size(); ++i) {
      bindings.push_back({names[i], derived_recipes::canonicalSeriesPath(sources_[i])});
    }
    return bindings;
  }
  const std::string& paramsText() const {
    return params_text_;
  }
  bool pinCurrentTime() const {
    return pin_current_;
  }
  /// Label of the "Show in 3D/2D" button offered for the recipe that was just created (empty hides it).
  void setSceneButton(std::string label) {
    scene_button_ = std::move(label);
  }
  /// Script body the on-demand chunk runs: one local per input (the Var column), the globals pane (if any)
  /// then the function body.
  std::string onDemandBody() const {
    return derived_recipes::buildVariablePrologue(language_, variableBindings()) +
           (global_code_.empty() ? function_body_ : global_code_ + "\n" + function_body_);
  }
  /// How many series a transform returns: the names listed in a saved comma-separated name, else the values its
  /// return statements give.
  std::size_t transformOutputCount() const {
    const std::size_t named = transformOutputNames(output_name_).size();
    return named != 0 ? named : returnArity(function_body_, language_);
  }
  /// The series a transform creates under `name`: the comma-separated names of an older state as they are, one name
  /// for one return value, `name/a`, `name/b`... for several.
  std::vector<std::string> transformOutputNames(const std::string& name) const {
    std::vector<std::string> listed = splitOutputNames(name);
    if (listed.size() > 1 || listed.empty()) {
      return listed;
    }
    const std::size_t count = returnArity(function_body_, language_);
    if (count == 1) {
      return listed;
    }
    std::vector<std::string> names;
    for (std::size_t k = 0; k < count; ++k) {
      names.push_back(listed.front() + "/" + static_cast<char>('a' + k));
    }
    return names;
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
  // One edit of the form (inputs, body, globals, Var, params, language): the preview runs again, and
  // Create waits for it (the last trial no longer describes what is on screen).
  void formChanged() {
    ++form_revision_;
    preview_dirty_ = true;
    clearTrial();
    needs_note_.clear();
    if (!body_edit_) {
      adaptDefaultBody();
    }
  }

  // The series template reads `value`, which an object input does not bind (it binds its Var). While the body is
  // still the untouched default, follow the inputs: `return <first var>` for an on-demand recipe, `return value`
  // again once the last object input is gone. A body the user edited (or loaded) is never touched.
  void adaptDefaultBody() {
    const std::string kDefault = "return value";
    if (isOnDemand() && !variableNames().empty()) {
      if (function_body_.empty() || function_body_ == kDefault ||
          (!auto_body_.empty() && function_body_ == auto_body_)) {
        auto_body_ = "return " + variableNames().front();
        function_body_ = auto_body_;
      }
    } else if (!auto_body_.empty()) {
      if (function_body_ == auto_body_) {
        function_body_ = kDefault;
      }
      auto_body_.clear();
    }
  }

  void storeTrial(std::optional<TrialReport> trial) {
    if (inferredOutputs() != (trial ? trial->outputs : std::vector<OnDemandOutput>{})) {
      ++trial_serial_;
    }
    trial_ = std::move(trial);
  }

  // What the form's edits determine, computed once per revision: the engine (an input's type is a catalog
  // lookup), the Var of every input, the params verdict and the problems that stop Create.
  struct Derived {
    std::uint64_t revision = 0;
    RecipeKind kind = RecipeKind::kTransform;
    std::vector<std::string> vars;
    std::string params_error;
    std::vector<std::string> problems;
  };
  const Derived& derived() const {
    if (derived_ && derived_->revision == form_revision_) {
      return *derived_;
    }
    Derived d;
    d.revision = form_revision_;
    std::vector<std::string> input_types;
    for (const std::string& source : sources_) {
      input_types.push_back(inputTypeOf(source));
    }
    d.kind = deduceEngine(input_types, kind_hint_on_demand_);
    if (d.kind == RecipeKind::kOnDemand) {
      d.vars = onDemandVariableNames(var_overrides_);
    } else {
      d.vars.resize(sources_.size());
      for (std::size_t i = 0; i < sources_.size(); ++i) {
        d.vars[i] = variableName(static_cast<int>(i));
      }
    }
    d.params_error = paramsError(params_text_);
    if (sources_.empty()) {
      d.problems.push_back(kNeedInputProblem);
    }
    if (function_body_.empty()) {
      d.problems.push_back(kNeedBodyProblem);
    }
    if (d.kind == RecipeKind::kOnDemand) {
      if (!on_demand_supported_) {
        d.problems.push_back(kNeedNewHostProblem);
      }
      if (!d.params_error.empty()) {
        d.problems.push_back(d.params_error);
      }
    }
    derived_ = std::move(d);
    return *derived_;
  }

  static bool sameSeries(const std::vector<PJ::ChartSeries>& a, const std::vector<PJ::ChartSeries>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PJ::ChartSeries& x, const PJ::ChartSeries& y) {
      return x.label == y.label && x.color == y.color && x.dashed == y.dashed &&
             std::equal(
                 x.points.begin(), x.points.end(), y.points.begin(), y.points.end(),
                 [](const PJ::ChartPoint& p, const PJ::ChartPoint& q) { return p.x == q.x && p.y == q.y; });
    });
  }

  // The saved-state hint lasts until the first change to the inputs.
  void inputsChanged() {
    kind_hint_on_demand_ = false;
    formChanged();
  }

  // The status line is one line: the entries of a multi-line message are joined by a middle dot.
  static std::string oneLine(std::string text) {
    std::string out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty()) {
        out += (out.empty() ? "" : " · ") + line;
      }
    }
    return out;
  }

  // Add an input row (no duplicates). The first row of a transform becomes its `value`.
  void addSource(const std::string& item) {
    if (item.empty() || std::find(sources_.begin(), sources_.end(), item) != sources_.end()) {
      return;
    }
    const bool was_empty = sources_.empty();
    sources_.push_back(item);
    var_overrides_.push_back({});
    if (was_empty) {
      primary_index_ = 0;
    }
    inputsChanged();
  }

  // "number" for a scalar input; the builtin type for an object topic. Resolved by the toolbox
  // against the catalog, so a "dataset:" qualifier and an ambiguous name are read the way Create
  // reads them.
  std::string inputTypeOf(const std::string& source) const {
    return input_type_of_ ? input_type_of_(source) : "number";
  }

  // "Create...": ask for the name. The prefill is the last name, else the first output the trial learned.
  void openCreatePrompt() {
    save_stage_ = SaveStage::None;
    create_stage_ = CreateStage::Prompt;
    create_name_ = !output_name_.empty()        ? output_name_
                   : !inferredOutputs().empty() ? inferredOutputs().front().name
                                                : std::string("result");
    pending_create_name_ = create_name_;
    create_note_ = replaceNote(create_name_);
    emit_create_dialog_ = true;
  }

  // "Replaces ..." when the name is already one of the editor's own recipes.
  std::string replaceNote(const std::string& name) const {
    return own_recipe_exists_ && own_recipe_exists_(name)
               ? "A recipe named '" + name + "' already exists: OK replaces it."
               : std::string{};
  }

  // OK in the prompt. A name that cannot be used, or that would replace a recipe and was not confirmed
  // by pressing OK a second time, opens the prompt again with the reason.
  void acceptCreatePrompt() {
    const std::string name = trimBlanks(pending_create_name_);
    create_stage_ = CreateStage::None;
    create_name_ = name;
    if (const std::string error = createNameError(name); !error.empty()) {
      create_note_ = error;
      create_stage_ = CreateStage::Prompt;
      emit_create_dialog_ = true;
      return;
    }
    if (own_recipe_exists_ && own_recipe_exists_(name) && replace_confirmed_ != name) {
      replace_confirmed_ = name;
      create_note_ = "A recipe named '" + name + "' already exists: press OK again to replace it.";
      create_stage_ = CreateStage::Prompt;
      emit_create_dialog_ = true;
      return;
    }
    replace_confirmed_.clear();
    output_name_ = name;
    pending_create_ = PendingCreate::Single;
  }

  // The kind-specific part of loadConfig. `cfg` is the editor state; `user_params_text` the
  // params object stored next to it (empty for a transform). A trial re-infers the outputs on load.
  void loadKindState(const nlohmann::json& cfg, const std::string& user_params_text) {
    params_text_.clear();
    pin_current_ = false;
    scene_button_.clear();
    kind_hint_on_demand_ = false;
    clearTrial();
    status_text_.clear();
    readout_.clear();
    needs_note_.clear();
    if (cfg.value("kind", std::string{}) == "on_demand") {
      kind_hint_on_demand_ = true;
      params_text_ = cfg.contains("params_text") && cfg["params_text"].is_string()
                         ? cfg["params_text"].get<std::string>()
                         : user_params_text;
      pin_current_ = cfg.value("pin_current_time", false);
    }
    advanced_open_ = !params_text_.empty() || pin_current_;
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
  bool body_edit_ = false;  // inside the handler of an edit of the body
  std::string auto_body_;   // the body the editor wrote for the first object input ("" = none), see adaptDefaultBody
  std::vector<std::string> sources_;
  int primary_index_ = -1;
  // On-demand form state (see isOnDemand()).
  bool kind_hint_on_demand_ = false;  // a loaded on-demand state: stays on-demand until an input changes
  std::vector<std::string>
      var_overrides_;        // parallel to sources_: the Var the user (or a library function) set, "" = default
  std::string params_text_;  // the params JSON field, verbatim
  bool pin_current_ = false;
  bool advanced_open_ = false;  // the Advanced disclosure (params, pin)
  bool on_demand_supported_ = true;
  // The last trial evaluation (see setTrial) and what is shown from it; empty before the first one and
  // after an edit.
  std::optional<TrialReport> trial_;
  std::uint64_t trial_serial_ = 0;
  std::uint64_t form_revision_ = 1;  // see formRevision()
  mutable std::optional<Derived> derived_;
  std::string status_text_;
  std::string readout_;
  std::string needs_note_;        // "needs: cloud (point cloud)" after a library function found no input for a variable
  std::string scene_embed_kind_;  // the embedded scene view of the preview; empty = none
  std::vector<PJ::SceneTopic> scene_embed_topics_;
  bool preview_has_numbers_ = true;
  std::string scene_button_;  // label of the "Show in 3D/2D" button; empty hides it
  bool show_scene_requested_ = false;
  std::function<void()> on_show_scene_;
  std::function<std::string(const std::string&)> input_type_of_;
  std::function<bool(const std::string&)> own_recipe_exists_;
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

  // "Create..." prompt (the name prompt of a new recipe; Modify has none).
  enum class CreateStage { None, Prompt };
  CreateStage create_stage_ = CreateStage::None;
  std::string create_name_;          // prefill of the prompt
  std::string pending_create_name_;  // what the user typed (harvested on OK)
  std::string create_note_;          // refusal or replace remark shown in the prompt
  std::string replace_confirmed_;    // the name whose replacement the user already confirmed
  bool emit_create_dialog_ = false;

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
  int preview_refresh_ticks_ = 0;
#ifdef PJ_TARGET_WASM
  static constexpr int kPreviewRefreshTickInterval = 20;
#else
  static constexpr int kPreviewRefreshTickInterval = 1;
#endif
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
  TransformEditorToolbox() {
    dialog_.setInputTypeResolver([this](const std::string& source) { return inputTypeOf(source); });
    dialog_.setOwnRecipeLookup([this](const std::string& name) { return ownRecipeExists(name); });
  }

  // The editor has no Close button; it is dismissed via the host panel chrome, which destroys this plugin
  // instance and removes the ephemeral previews it owns. What is left to release is the evaluation in
  // flight (tearDownPreview() is a no-op when none is live).
  ~TransformEditorToolbox() override {
    tearDownPreview();
  }

  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    if (!callbacks_wired_) {
      dialog_.setOnSave([this]() { onSave(); });
      dialog_.setOnShowScene([this]() { showInScene(); });
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
    // Optional: anchors an on-demand preview's instant to the playhead.
    if (auto pb = services.get<PJ::sdk::PlaybackHostService>()) {
      playback_view_ = *pb;
    }
    // Optional: "Show in 3D/2D" after creating an on-demand recipe.
    if (auto tabs = services.get<PJ::sdk::PlotTabHostService>()) {
      plot_tabs_view_ = *tabs;
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
  friend class TransformEditorPreviewTestPeer;

  void onSave() {
    // The engine follows the input types, so read the catalog now: Create acts on what the user sees.
    refreshOnDemandSupport(/*force=*/true);
    if (const std::string reason = dialog_.canCreateReason(); !reason.empty()) {
      report(PJ::ToolboxMessageLevel::kWarning, "Transform Editor: " + reason);
      return;
    }
    if (dialog_.isOnDemand()) {
      onSaveOnDemand();
      return;
    }
    const auto& source = dialog_.sourceSeries();
    const auto& output_name = dialog_.outputName();
    const auto& global = dialog_.globalCode();
    const auto& body = dialog_.functionBody();

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

    // One output for one returned value; `name/a`, `name/b`... for several (an older state's
    // comma-separated names stay as they are). The body must `return` one value per output.
    std::vector<std::string> output_names = dialog_.transformOutputNames(output_name);
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

  // True when `name` is already one of the editor's OWN recipes (installed with its state in the
  // params), so Create would replace it. Recipes of the assistant or of other plugins never count.
  bool ownRecipeExists(const std::string& name) {
    if (!dp_view_.valid()) {
      return false;
    }
    const std::vector<std::string> names =
        dialog_.isOnDemand() ? std::vector<std::string>{name} : dialog_.transformOutputNames(name);
    if (names.empty()) {
      return false;
    }
    const std::string& id = names.front();
    auto ids = dp_view_.list();
    if (!ids || std::find(ids->begin(), ids->end(), id) == ids->end()) {
      return false;
    }
    auto config = dp_view_.recipeOf(id);
    if (!config) {
      return false;
    }
    const auto parsed = nlohmann::json::parse(*config, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object() || !parsed.contains("params") || !parsed["params"].is_object()) {
      return false;
    }
    const auto& params = parsed["params"];
    return params.contains(kEditorParamsKey) || params.contains("function_body");
  }

  // The catalog the on-demand features read: the toolbox host, or a test double.
  [[nodiscard]] PJ::sdk::ToolboxHostView catalogHost() const {
    return test_catalog_host_.valid() ? test_catalog_host_ : toolboxHost();
  }

  // The catalog snapshot the on-demand form works from, (inputs are added by drag and drop).
  // Re-read every kCatalogRefreshTicks calls (the form's own edits never need a fresh read);
  // `force` re-reads now. Whether the host can do on-demand at all (the typed data-processor
  // surface plus catalog snapshot v2) is decided here too, and any re-read drops the cached build.
  void refreshOnDemandSupport(bool force = false) {
    if (!force && catalog_refresh_ticks_ > 0 && catalog_refresh_ticks_ < kCatalogRefreshTicks) {
      ++catalog_refresh_ticks_;
      return;
    }
    catalog_refresh_ticks_ = 1;
    catalog_v2_.reset();
    catalog_v2_error_.clear();
    catalog_scalar_paths_.reset();
    if (dp_view_.hasTypedRequests()) {
      auto v2 = catalogHost().catalogSnapshotV2();
      if (v2) {
        catalog_v2_ = std::move(*v2);
      } else {
        catalog_v2_error_ = std::string(v2.error());
      }
    }
    if (!catalog_v2_ && catalogHost().valid() && !catalogHost().hasCatalogSnapshotV2()) {
      catalog_scalar_paths_ = readScalarPaths();
    }
    dialog_.setOnDemandSupported(catalog_v2_.has_value());
    dialog_.catalogChanged();  // the input types, and so the build, may differ
  }

  // Every scalar topic and `topic/field` path of the host's v1 catalog snapshot.
  std::set<std::string> readScalarPaths() const {
    std::set<std::string> paths;
    auto catalog = catalogHost().catalogSnapshot();
    if (!catalog) {
      return paths;
    }
    const auto fields = catalog->fields();
    for (const auto& t : catalog->topics()) {
      const std::string tname(t.name.data, t.name.size);
      paths.insert(tname);
      const uint32_t end = t.first_field + t.field_count;
      for (uint32_t fi = t.first_field; fi < end && fi < fields.size(); ++fi) {
        const std::string fname(fields[fi].name.data, fields[fi].name.size);
        const std::string leaf = (!fname.empty() && fname.front() == '/') ? fname.substr(1) : fname;
        paths.insert(tname.empty() ? leaf : tname + "/" + leaf);
      }
    }
    return paths;
  }

  // "number" for a scalar input; the builtin type for an object topic (read from the cached catalog). A
  // host without catalog snapshot v2 cannot list object topics: what is not a scalar there is "unknown".
  std::string inputTypeOf(const std::string& source) const {
    if (!catalog_v2_) {
      if (catalog_scalar_paths_) {
        const std::string path = derived_recipes::canonicalSeriesPath(source);
        return catalog_scalar_paths_->count(path) != 0 || catalog_scalar_paths_->count(source) != 0 ? "number"
                                                                                                    : kUnknownInputType;
      }
      return "number";
    }
    const auto lookup = derived_recipes::resolveObjectTopic(*catalog_v2_, derived_recipes::canonicalSeriesPath(source));
    if (lookup.ambiguous) {
      return "ambiguous";
    }
    return lookup.resolved ? lookup.resolved->object_type : "number";
  }

  // The first entry of the first object input (its time range comes from the catalog): where a trial
  // runs when the cursor is before every sample.
  struct FirstEntry {
    std::int64_t ns = 0;
    std::string topic;  // the input as the user named it
  };

  // What the form asks the host to run: the request (inputs resolved to request paths, the generated
  // script chunk in the selected language, the user's params; the outputs are left for the host to infer)
  // and the dataset the playhead is anchored to. Create and the preview both start from it, so the
  // preview runs what Create installs. `error` is a message for the user; `form_incomplete` marks the
  // cases the form's own hints already explain (bad params), which the preview leaves silent.
  // `trial_request` is the request a trial submits (the id, flags and no outputs set; the instant is
  // added per run), `form_key` its signature and `data_stamp` the (entry count, last time) of every
  // object input at the catalog read the build was made from: they change when the input data grows.
  struct OnDemandBuild {
    PJ::sdk::DataProcessorRequest request;
    PJ::sdk::DataProcessorRequest trial_request;
    std::string form_key;
    std::optional<PJ::sdk::DataSourceHandle> anchor;
    std::optional<FirstEntry> first_entry;
    std::vector<std::pair<std::uint64_t, std::int64_t>> data_stamp;
    std::string error;
    bool form_incomplete = false;
  };

  // The trial's own form of `build.request` and its signature.
  static void setTrialForm(OnDemandBuild& build) {
    build.trial_request = build.request;
    makeEphemeral(build.trial_request, kPreviewId, std::nullopt);
    build.trial_request.flags |= PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS;
    build.form_key = requestSignature(build.trial_request);
  }

  // Rebuilt only when the form changes or the catalog is re-read (both bump the dialog's form revision);
  // otherwise the cached build is returned.
  const OnDemandBuild& buildOnDemandRequest() {
    if (on_demand_build_ && on_demand_build_revision_ == dialog_.formRevision()) {
      return *on_demand_build_;
    }
    on_demand_build_revision_ = dialog_.formRevision();
    ++on_demand_build_count_;
    OnDemandBuild build;
    build.request.kind = "on_demand";
    build.request.language = dialog_.language() == "python" ? "python" : "luau";
    if (!catalog_v2_) {
      build.error = "The host's object catalog is unavailable" +
                    (catalog_v2_error_.empty() ? std::string() : ": " + catalog_v2_error_);
    } else if (const std::string& params_error = dialog_.paramsProblem(); !params_error.empty()) {
      build.error = params_error;
      build.form_incomplete = true;
    } else {
      std::vector<std::string> raw;
      for (const std::string& source : dialog_.sources()) {
        raw.push_back(derived_recipes::canonicalSeriesPath(source));
      }
      PJ::sdk::ToolboxHostView host = catalogHost();
      const auto resolved =
          derived_recipes::resolveEvalInputs(host, *catalog_v2_, raw, derived_recipes::Audience::kUser);
      if (!resolved.error.empty()) {
        build.error = resolved.error;
      } else {
        for (const auto& input : resolved.inputs) {
          build.request.inputs.push_back(input.request_path);
        }
        const std::string body = dialog_.onDemandBody();
        build.request.script = build.request.language == "python"
                                   ? derived_recipes::buildOnDemandChunkPython(body, resolved)
                                   : derived_recipes::buildResolvedOnDemandChunk(body, resolved);
        build.request.params_json = parseParamsObject(dialog_.paramsText())->dump();
        build.anchor = resolved.anchor_source;
        for (std::size_t i = 0; i < resolved.inputs.size(); ++i) {
          const auto& input = resolved.inputs[i];
          if (!input.is_object) {
            continue;
          }
          build.data_stamp.emplace_back(input.entry_count, input.time_max_ns);
          if (!build.first_entry && input.entry_count > 0) {
            build.first_entry = FirstEntry{input.time_min_ns, dialog_.sources()[i]};
          }
        }
        setTrialForm(build);
      }
    }
    on_demand_build_ = std::move(build);
    return *on_demand_build_;
  }

  void report(PJ::ToolboxMessageLevel level, const std::string& msg) {
    if (runtimeHostBound()) {
      runtimeHost().reportMessage(level, msg);
    }
  }

  // Create / Modify an on_demand recipe: the same request the assistant's create_derived_object
  // builds (shared helpers), with the outputs the trial inferred (INFER_OUTPUTS: the host binds them by
  // name, positional returns by position, and stays strict), the user's params and the editor state for
  // the pencil.
  void onSaveOnDemand() {
    const std::string id = trimBlanks(dialog_.outputName());
    if (id.empty()) {
      report(PJ::ToolboxMessageLevel::kWarning, "Transform Editor: a name is required.");
      return;
    }
    const OnDemandBuild& build = buildOnDemandRequest();
    if (!build.error.empty()) {
      report(PJ::ToolboxMessageLevel::kError, "Transform Editor: " + build.error);
      return;
    }
    if (auto valid = dp_view_.validateScript("on_demand", build.request.language, build.request.script); !valid) {
      report(PJ::ToolboxMessageLevel::kError, "Transform Editor: invalid script: " + std::string(valid.error()));
      return;
    }

    PJ::sdk::DataProcessorRequest request = build.request;
    request.id = id;
    request.label = id;
    // The user's own recipe (not EPHEMERAL): undo/redo applies like it does to their transforms.
    request.flags = PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS;
    for (const auto& output : dialog_.inferredOutputs()) {
      request.outputs.push_back({output.name, output.type});
    }
    if (dialog_.pinCurrentTime()) {
      std::optional<std::int64_t> pinned;
      if (build.anchor) {
        if (auto state = playback_view_.state()) {
          pinned = derived_recipes::toRawNs(playback_view_, *build.anchor, state->current_time_s);
        }
      }
      if (!pinned) {
        report(
            PJ::ToolboxMessageLevel::kError,
            "Transform Editor: cannot pin at the current time: the host did not expose playback or per-source time "
            "conversion.");
        return;
      }
      request.instant_ns = pinned;
    }
    // The script receives the user's params; kEditorParamsKey carries the editor's state so the
    // host's pencil can reopen this recipe (the script ignores it). buildOnDemandRequest refused
    // params that already use the key.
    nlohmann::json params = nlohmann::json::parse(request.params_json);
    params[kEditorParamsKey] = nlohmann::json::parse(dialog_.saveConfig(), nullptr, /*allow_exceptions=*/false);
    request.params_json = params.dump();

    auto created = dp_view_.createV2(request);
    if (!created) {
      report(PJ::ToolboxMessageLevel::kError, "Transform Editor: " + std::string(created.error()));
      return;
    }
    report(PJ::ToolboxMessageLevel::kInfo, "Transform Editor: saved '" + id + "'.");
    tearDownObjectPreview();  // the created outputs replace the preview
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }

    // Offer to open the object outputs in a scene tab; otherwise the editor closes like Create does.
    scene_topics_.clear();
    scene_kind_.clear();
    if (plot_tabs_view_.hasSceneTabs() && created->size() == request.outputs.size()) {
      SceneTargets targets = collectSceneTargets(request.outputs, *created, build.anchor);
      scene_topics_ = std::move(targets.topics);
      scene_kind_ = std::move(targets.kind);
    }
    if (!scene_topics_.empty()) {
      scene_id_ = std::string(kSceneIdPrefix) + id;
      dialog_.setSceneButton(scene_kind_ == "2d" ? "Show in 2D" : "Show in 3D");
    } else {
      dialog_.requestClose();
    }
  }

  // The object outputs of a created recipe with the dataset each one lives in, and the one scene kind that
  // shows them: 3D unless every object output is a 2D one. The dataset comes from the catalog like the assistant's
  // scene_view attach does; a topic the catalog does not list yet takes the dataset its inputs were anchored to.
  struct SceneTargets {
    std::vector<PJ::SceneTopic> topics;
    std::string kind;  // "3d" unless every object output is 2D; empty without object outputs
  };
  SceneTargets collectSceneTargets(
      const std::vector<PJ::sdk::DataProcessorOutput>& outputs, const std::vector<std::string>& created,
      std::optional<PJ::sdk::DataSourceHandle> anchor) {
    SceneTargets targets;
    auto v2 = catalogHost().catalogSnapshotV2();
    if (!v2) {
      return targets;
    }
    const auto listed = derived_recipes::listObjectTopics(*v2);
    bool all_2d = true;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (!isObjectOutputType(outputs[i].type)) {
        continue;
      }
      const auto it = std::find_if(listed.begin(), listed.end(), [&](const auto& e) { return e.name == created[i]; });
      std::string dataset;
      if (it != listed.end()) {
        dataset = it->dataset;
      } else if (anchor) {
        dataset = derived_recipes::objectTopicDatasetName(v2->dataSources(), *anchor);
      }
      targets.topics.push_back({created[i], std::move(dataset)});
      all_2d = all_2d && sceneKindForOutputType(outputs[i].type) == "2d";
    }
    if (!targets.topics.empty()) {
      targets.kind = all_2d ? "2d" : "3d";
    }
    return targets;
  }

  // "Show in 3D/2D": a scene tab of the right kind with the created object topics attached.
  void showInScene() {
    if (!plot_tabs_view_.hasSceneTabs() || scene_topics_.empty()) {
      return;
    }
    const std::string title = scene_id_.substr(kSceneIdPrefix.size());
    if (auto status = plot_tabs_view_.createTabV2(scene_id_, scene_kind_, title); !status) {
      report(PJ::ToolboxMessageLevel::kError, "Transform Editor: " + std::string(status.error()));
      return;
    }
    attachTopics(scene_id_, scene_topics_);
    (void)plot_tabs_view_.focusTab(scene_id_);
  }

  // Attach each topic to the scene tab; a failure is reported as an error.
  void attachTopics(std::string_view tab_id, const std::vector<PJ::SceneTopic>& topics) {
    for (const auto& [topic, dataset] : topics) {
      if (auto status = plot_tabs_view_.attachTopic(tab_id, topic, dataset); !status) {
        report(
            PJ::ToolboxMessageLevel::kError,
            "Transform Editor: could not attach '" + topic + "': " + std::string(status.error()));
      }
    }
  }

  // Batch create: apply the batch function to EVERY input series, naming each
  // output with the prefix/suffix (mirrors the native panel's batch path). One
  // transform per source; each output is surfaced in Custom Series via on_data_changed.
  void onSaveBatch() {
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

  // The preview recipe of a trial that inferred its outputs: an EPHEMERAL on_demand recipe (it evaluates at
  // the playhead, so the preview follows the cursor) with the inferred outputs and INFER_OUTPUTS. Its
  // object topics show in the scene view embedded in the editor; each number output is plotted from the
  // series the host materializes for it (a newer host) or, without it, shown as the readout at the cursor.
  // The recipe is re-upserted in place when the form changes (at most every kPreviewRefresh); it is
  // removed with the editor's panel (by the host), on Create, or when the trial fails. Without an
  // embedding host there is nothing to show for objects beyond the status line.
  void updatePreviewRecipe(const OnDemandBuild& build) {
    if (!dp_view_.hasTypedRequests() || dialog_.inferredOutputs().empty()) {
      tearDownObjectPreview();
      return;
    }
    // Objects preview in a scene view embedded next to the plot when the dialog host binds a `scene_view`
    // frame to a live view; otherwise the frame stays hidden and only the status line and readout show.
    const bool embedded = dialog_.embedsSceneViews();
    // The recipe follows the form and the trial's outputs: nothing to rebuild or compare while neither moved.
    const std::pair<std::uint64_t, std::uint64_t> stamp{dialog_.formRevision(), dialog_.trialSerial()};
    if (stamp != preview_recipe_stamp_) {
      PJ::sdk::DataProcessorRequest request = build.request;
      for (const auto& output : dialog_.inferredOutputs()) {
        request.outputs.push_back({output.name, output.type});
      }
      makeEphemeral(request, kObjectPreviewId, std::nullopt);  // no instant: follows the cursor
      request.flags |= PJ_DATA_PROCESSOR_FLAG_INFER_OUTPUTS;
      const bool has_object = std::any_of(
          request.outputs.begin(), request.outputs.end(),
          [](const PJ::sdk::DataProcessorOutput& o) { return isObjectOutputType(o.type); });
      const std::string key = requestSignature(request);
      if (key != object_preview_signature_) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_object_preview_refresh_) {
          return;  // an edit just landed: the next tick installs the latest form
        }
        next_object_preview_refresh_ = now + kPreviewRefresh;
        object_preview_signature_ = key;
        // Re-upserting the id is an edit for the host: the topics stay and the bound scene layer with them.
        auto created = dp_view_.createV2(request);
        if (!created) {
          dialog_.setEmbeddedScene({}, {});
          report(PJ::ToolboxMessageLevel::kWarning, "Transform Editor: preview: " + std::string(created.error()));
          return;
        }
        object_preview_live_ = true;
        object_preview_topics_ = *created;
        ++object_preview_installs_;
        if (has_object && embedded) {
          showEmbeddedScene(request, *created, build.anchor);
        } else {
          dialog_.setEmbeddedScene({}, {});  // the outputs are no longer objects
        }
      }
      preview_recipe_stamp_ = stamp;
      preview_recipe_outputs_ = std::move(request.outputs);
    }
    showSeriesOrReadout(preview_recipe_outputs_, build);
  }

  // The object outputs of the installed preview recipe in the scene view embedded in the editor. One view
  // for now, of the kind collectSceneTargets picks.
  void showEmbeddedScene(
      const PJ::sdk::DataProcessorRequest& request, const std::vector<std::string>& created,
      std::optional<PJ::sdk::DataSourceHandle> anchor) {
    SceneTargets targets;
    if (created.size() == request.outputs.size()) {
      targets = collectSceneTargets(request.outputs, created, anchor);
    }
    dialog_.setEmbeddedScene(std::move(targets.kind), std::move(targets.topics));
  }

  // Plot the number outputs of the preview recipe over the whole history when the host materializes them
  // (the catalog key is `<recipe key>/<output name>`); otherwise the readout at the cursor and a note. Read
  // at most every kSeriesRead, and only when there is something new to read: it scans the catalog.
  void showSeriesOrReadout(const std::vector<PJ::sdk::DataProcessorOutput>& outputs, const OnDemandBuild& build) {
    std::vector<std::size_t> numbers;  // the index in `outputs` of each number output
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      if (outputs[i].type == "number") {
        numbers.push_back(i);
      }
    }
    dialog_.setPreviewHasNumbers(!numbers.empty());
    if (numbers.empty()) {
      dialog_.setPreviewSeries({});
      dialog_.setReadout({});  // object outputs show in the scene view
      return;
    }
    // New data to read: the recipe was just (re)installed or an object input grew. Until the host
    // materializes a series (an older one never does) and for inputs the catalog says nothing about
    // (scalars), every period is a chance to find one.
    const SeriesStamp stamp{object_preview_installs_, build.data_stamp};
    const bool due = !series_available_ || build.data_stamp.empty() || !(series_read_stamp_ == stamp);
    const auto now = std::chrono::steady_clock::now();
    if (due && now >= next_series_read_) {
      next_series_read_ = now + kSeriesRead;
      // The host returned the catalog path of each output's series (`<owner>/<id>/<name>`) when it creates
      // the preview recipe; read exactly that, never a name guessed from the output.
      std::vector<std::string> names;
      for (const std::size_t index : numbers) {
        names.push_back(index < object_preview_topics_.size() ? object_preview_topics_[index] : std::string{});
      }
      const auto samples = readManyRawSamples(names);
      std::vector<RawSamples> found;
      std::vector<std::string> labels;
      for (std::size_t i = 0; i < numbers.size(); ++i) {
        if (!samples[i].empty()) {
          found.push_back(samples[i]);
          labels.push_back(outputs[numbers[i]].name);
        }
      }
      std::vector<PJ::ChartSeries> series = toChartSeries(found, labels, earliestTimestamp(found));
      series_available_ = !series.empty();
      series_read_stamp_ = stamp;
      dialog_.setPreviewSeries(std::move(series));  // keeps the plotted series when nothing changed
    }
    const std::string& readout = dialog_.trialReadout();
    dialog_.setReadout(series_available_ ? readout : readout + "\nseries preview needs a newer host");
  }

  // Remove the ephemeral object recipe and clear the embedded scene view.
  void tearDownObjectPreview() {
    if (object_preview_live_ && dp_view_.valid()) {
      (void)dp_view_.remove(kObjectPreviewId);
    }
    object_preview_live_ = false;
    object_preview_signature_.clear();
    object_preview_topics_.clear();
    preview_recipe_stamp_ = {};
    next_object_preview_refresh_ = {};
    series_available_ = false;
    dialog_.setEmbeddedScene({}, {});
    dialog_.setPreviewHasNumbers(true);
    dialog_.setReadout({});
  }

  void tearDownPreview() {
    if (pending_preview_ && dp_view_.valid()) {
      (void)dp_view_.releaseEvaluation(*pending_preview_);
    }
    pending_preview_.reset();
    trial_form_key_.clear();  // whatever comes next is a fresh run
    if (!preview_key_.empty() && dp_view_.valid()) {
      (void)dp_view_.remove(preview_key_);
      preview_key_.clear();
    }
  }

  // The ephemeral transform output lives in the engine's topic list (and hence in catalogSnapshot) even
  // though it is kept out of the UI catalog, so the readers below resolve both the source AND the
  // transformed result by name.
  // Decimated (timestamp, value) samples for one already-resolved field handle.
  std::vector<std::pair<int64_t, double>> samplesFromHandle(PJ::sdk::FieldHandle handle) {
    std::vector<std::pair<int64_t, double>> out;
    auto read = catalogHost().readSeries(handle);
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

  // Resolve several "topic/field" names (or a bare topic) against ONE
  // catalog snapshot in a single nested pass, returning decimated samples per name
  // (empty if a name did not resolve). The MIMO preview reads the source ghost + M
  // outputs every tick; a per-name readRawSamples would re-acquire the snapshot and
  // re-scan the whole catalog M+1 times. Field paths in the snapshot are RELATIVE to
  // their topic and a ROS leaf carries a leading '/', so reconstruct "topic/field"
  // WITHOUT doubling the slash (matching the host's joinTopicField) — a naive
  // `tname + "/" + fname` would rebuild the old "topic//field" and fail to match.
  std::vector<std::vector<std::pair<int64_t, double>>> readManyRawSamples(const std::vector<std::string>& names) {
    std::vector<std::vector<std::pair<int64_t, double>>> out(names.size());
    auto catalog = catalogHost().catalogSnapshot();
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
          if (found[i] || names[i].empty() || (tname != names[i] && full != names[i])) {
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

  // "t = 12.3 s" in the playback's display time of the anchor dataset (raw seconds without it).
  std::string displayTimeText(
      std::optional<PJ::sdk::DataSourceHandle> anchor, std::int64_t raw_ns, int fixed_decimals = -1) {
    double seconds = static_cast<double>(raw_ns) * 1e-9;
    if (anchor && playback_view_.valid()) {
      if (auto shown = playback_view_.toDisplayTimeForSource(*anchor, raw_ns)) {
        seconds = *shown;
      }
    }
    std::ostringstream out;
    if (fixed_decimals >= 0) {
      out << std::fixed << std::setprecision(fixed_decimals) << seconds;
    } else {
      out << std::setprecision(6) << seconds;
    }
    return out.str();
  }

  // The host's verdict on a script, asked once per form revision: compiling it is not free and the form does
  // not change between ticks. nullopt when the script is accepted, else the host's message. `make_script`
  // builds the script only when the host is asked.
  template <typename MakeScript>
  std::optional<std::string> scriptError(std::string_view kind, const std::string& language, MakeScript&& make_script) {
    if (!validation_ || validation_->revision != dialog_.formRevision() || validation_->kind != kind) {
      auto verdict = dp_view_.validateScript(kind, language, make_script());
      validation_ = Validation{
          dialog_.formRevision(), std::string(kind),
          verdict ? std::optional<std::string>{} : std::string(verdict.error())};
    }
    return validation_->error;
  }

  // The trial of an on-demand recipe: ONE evaluation, EPHEMERAL, with the outputs left for the host to
  // infer (INFER_OUTPUTS), at the playhead's raw instant. `build` carries the request (inputs as request paths,
  // script, language and params) and the trial's own form of it; the instant is set here. A trial runs 300 ms
  // after the last edit and, for the readout at the cursor, at most every kTrialPeriod after that. When the
  // cursor has no sample it runs once more at the first entry of the first object input (`build.first_entry`).
  // Returns false when the host rejected the script (the error is on the dialog).
  bool previewOnDemand(const OnDemandBuild& build) {
    if (const auto invalid = scriptError("on_demand", build.request.language, [&] { return build.request.script; })) {
      tearDownPreview();
      dialog_.setTrialFailure(*invalid);
      dialog_.setValidationError(*invalid);
      return false;
    }
    const auto& anchor = build.anchor;
    const auto& first_entry = build.first_entry;
    // The playhead's raw instant in the anchor input's dataset (shared toRawNs: one raw(0)->display
    // conversion inverted).
    std::int64_t instant_ns = 0;
    std::string note = "no playback service: evaluating at 0 s";
    if (anchor) {
      if (auto state = playback_view_.state()) {
        if (auto raw = derived_recipes::toRawNs(playback_view_, *anchor, state->current_time_s)) {
          instant_ns = *raw;
          note.clear();
        }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    // An edit obsoletes any evaluation in flight and waits for the debounce; the cursor alone does not.
    if (build.form_key != trial_form_key_) {
      tearDownPreview();
      trial_form_key_ = build.form_key;
      trial_edit_at_ = now;
      trial_form_pending_ = true;
      no_sample_instant_.reset();
    }
    if (trial_form_pending_ && now < trial_edit_at_ + edit_debounce_) {
      return true;
    }
    const bool at_first = first_entry && no_sample_instant_ && *no_sample_instant_ == instant_ns;
    if (!pending_preview_ && (trial_form_pending_ || now >= next_preview_refresh_)) {
      tearDownPreview();
      trial_form_key_ = build.form_key;
      trial_form_pending_ = false;
      next_preview_refresh_ = now + kTrialPeriod;
      dialog_.setValidationError("");
      PJ::sdk::DataProcessorRequest request = build.trial_request;
      request.instant_ns = at_first ? first_entry->ns : instant_ns;
      const PJ::sdk::EvaluationBudget budget{.max_millis = 1000};
      auto handle = dp_view_.submitEvaluation(request, budget);
      if (!handle) {
        dialog_.setTrialFailure(std::string(handle.error()));
        dialog_.setValidationError(std::string(handle.error()));
        return true;
      }
      pending_preview_ = *handle;
      preview_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }
    if (!pending_preview_) {
      return true;  // Reuse the last report briefly; a paused live input may still change.
    }
    auto attempt = dp_view_.pollEvaluation(*pending_preview_);
    if (attempt && attempt->state == PJ::sdk::EvaluationState::kPending &&
        std::chrono::steady_clock::now() < preview_deadline_) {
      return true;
    }
    (void)dp_view_.releaseEvaluation(*pending_preview_);
    pending_preview_.reset();
    next_preview_refresh_ = std::chrono::steady_clock::now() + kTrialPeriod;
    std::string error;
    if (!attempt) {
      error = attempt.error();
    } else if (attempt->state == PJ::sdk::EvaluationState::kFailed) {
      const auto err = nlohmann::json::parse(attempt->json, nullptr, /*allow_exceptions=*/false);
      error = err.is_object() && err.contains("error") && err["error"].is_string() ? err["error"].get<std::string>()
                                                                                   : attempt->json;
    } else if (attempt->state == PJ::sdk::EvaluationState::kCancelled) {
      error = "cancelled by the host";
    } else if (attempt->state != PJ::sdk::EvaluationState::kCompleted) {
      error = "timed out waiting for the host";
    }
    TrialReport trial;
    if (error.empty()) {
      trial = parseTrialReport(attempt->json);
      error = trial.error;
    }
    if (!error.empty()) {
      error = withInputsHint(error);
      dialog_.setTrialFailure(error);
      dialog_.setValidationError(error);
      dialog_.setStatus("");
      return true;
    }
    if (!trial.has_sample) {
      if (!at_first && first_entry && first_entry->ns != instant_ns) {
        // No sample at the cursor: try again, now, at the first entry of the first object input.
        no_sample_instant_ = instant_ns;
        next_preview_refresh_ = {};
        return previewOnDemand(build);
      }
      const std::string text = "No sample to run on: the inputs have no data";
      dialog_.setTrialFailure(text);
      dialog_.setValidationError("");
      dialog_.setStatus(text);
      return true;
    }
    if (trial.outputs.empty()) {
      const std::string text = withInputsHint("The script returned no values");
      dialog_.setTrialFailure(text);
      dialog_.setValidationError(text);
      return true;
    }
    dialog_.setValidationError("");
    if (at_first) {
      // The outputs were learned at the first entry; the embedded scene follows the real cursor and stays
      // empty, so the line says so and shows nothing measured at the retried instant.
      std::string outputs;
      for (const auto& output : trial.outputs) {
        outputs += (outputs.empty() ? "" : ", ") + output.name + " (" + objectTypeLabel(output.type) + ")";
      }
      trial.readout.clear();
      dialog_.setTrial(std::move(trial));
      dialog_.setStatus(
          "No sample of " + first_entry->topic + " at the cursor (" + displayTimeText(anchor, instant_ns, 3) +
          " s): move the timeline to preview. Outputs: " + outputs);
      return true;
    }
    const std::string result = std::move(trial.summary);
    dialog_.setTrial(std::move(trial));
    dialog_.setStatus(note.empty() ? result : note + "\n" + result);
    return true;
  }

  // What the host says when the script returns nothing, or fails on a nil, does not name the inputs. Say which
  // names are bound, and that `value` is one of them only for series inputs.
  std::string withInputsHint(const std::string& error) const {
    std::string lower = error;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool no_values = lower.find("returned no values") != std::string::npos;
    if (!no_values && lower.find("nil") == std::string::npos) {
      return error;
    }
    const auto& vars = dialog_.variableNames();
    std::string names;
    for (const std::string& var : vars) {
      names += (names.empty() ? "" : ", ") + var;
    }
    if (names.empty()) {
      return error;
    }
    const bool value_bound = std::find(vars.begin(), vars.end(), "value") != vars.end();
    if (!value_bound && mentionsIdentifier(dialog_.functionBody(), "value")) {
      return error + " · `value` is not defined here; inputs are: " + names;
    }
    return no_values ? error + " · inputs are: " + names : error;
  }

  // `word` as an identifier of `text` (not part of a longer name, not a field after `.` or `:`).
  static bool mentionsIdentifier(const std::string& text, const std::string& word) {
    auto is_name = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
      const bool before_ok = at == 0 || (!is_name(text[at - 1]) && text[at - 1] != '.' && text[at - 1] != ':');
      const std::size_t end = at + word.size();
      if (before_ok && (end >= text.size() || !is_name(text[end]))) {
        return true;
      }
    }
    return false;
  }

  void refreshPreview() {
    refreshOnDemandSupport();
    // The live preview belongs to the Single Function tab only. On the Batch tab
    // tear it down so its ephemeral node can never coexist with / leak into a
    // batch Create (and to avoid needless per-tick churn).
    if (dialog_.currentTab() != 0) {
      tearDownPreview();
      tearDownObjectPreview();
      dialog_.setStatus("");
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
      tearDownObjectPreview();
      dialog_.clearTrial();
      dialog_.setStatus("");
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

    // Object inputs (see previewOnDemand): the body is one evaluate-at-an-instant chunk, built with the
    // same helper the assistant uses so the preview runs what Create installs.
    if (dialog_.isOnDemand()) {
      if (!showing_on_demand_) {
        showing_on_demand_ = true;
        dialog_.setPreviewSeries({});  // the ghost of a transform preview is not ours
      }
      const OnDemandBuild& build = buildOnDemandRequest();
      if (!dialog_.onDemandSupported() || build.form_incomplete) {
        tearDownPreview();  // the form's hints say what is missing
        tearDownObjectPreview();
        dialog_.clearTrial();
        dialog_.setStatus("");
        return;
      }
      if (!build.error.empty()) {
        tearDownPreview();
        tearDownObjectPreview();
        dialog_.setTrialFailure(build.error);
        dialog_.setValidationError(build.error);
        dialog_.setStatus("");
        return;
      }
      if (!previewOnDemand(build) || dialog_.trialFailed()) {
        tearDownObjectPreview();
      } else if (dialog_.trialUsable()) {
        updatePreviewRecipe(build);
      }
      return;
    }
    showing_on_demand_ = false;
    tearDownObjectPreview();  // numeric outputs preview as a plot, below
    dialog_.clearTrial();
    dialog_.setStatus("");

    // Declare as many output topics as the body returns values (see transformOutputCount), so a MIMO body
    // returning M values matches the node's arity — a single-output preview node drops every row of an
    // M-output function and falsely reports "no output". Each slot gets a unique ephemeral name.
    const std::string preview_id(kPreviewId);
    const std::vector<std::string> output_names = dialog_.transformOutputNames(dialog_.outputName());
    const std::size_t num_outputs = dialog_.transformOutputCount();
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
    const auto invalid = scriptError("transform", dialog_.language(), [&] {
      return buildTransformScript("__validate__", "__validate__", global, body, num_extra, dialog_.language());
    });
    dialog_.setValidationError(invalid.value_or(""));
    if (invalid) {
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

    const std::int64_t t0 = earliestTimestamp(samples);  // before the ghost is moved out
    auto ghost_raw = std::move(samples[0]);
    std::vector<RawSamples> results(
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
      // The body must return EXACTLY as many values per point as the node has outputs (positional;
      // the count is read from its return statements). A silent MIMO no-op is almost always this
      // arity mismatch — in EITHER direction (too few OR too many returns) — or a
      // body that returns nothing. State the count symmetrically rather
      // than a misleading "must return a number" (the user may have returned several).
      const std::string n = std::to_string(num_outputs);
      const std::string plural = num_outputs == 1 ? "" : "s";
      const std::string msg = "function produced no output: it must return exactly " + n + " value" + plural +
                              " per point (counted from its return statements)";
      dialog_.setValidationError(msg);
    }

    std::vector<std::string> labels;
    for (std::size_t k = 0; k < results.size(); ++k) {
      if (k < output_names.size() && !output_names[k].empty()) {
        labels.push_back(output_names[k]);
      } else {
        labels.push_back(results.size() > 1 ? ("result" + std::to_string(k)) : std::string("result"));
      }
    }

    // Ghost (original): faded blue + dashed, distinct from the solid result curves
    // — same styling as the native TransformEditorPanel. Color hex is #AARRGGBB.
    std::vector<PJ::ChartSeries> series;
    series.push_back({source, toChartPoints(ghost_raw, t0), "#5A4488ff", /*dashed=*/true});
    // One solid curve per declared output, cycling the palette so parallel MIMO outputs stay visually
    // distinct; labelled by the user's output name.
    for (auto& solid : toChartSeries(results, labels, t0)) {
      series.push_back(std::move(solid));
    }
    dialog_.setPreviewSeries(std::move(series));
  }

  // Validate the BATCH function via the SDK's validateScript — the host compiles
  // it and runs a synthetic test point, with NO node/topic created (unlike the old
  // throwaway-transform approach). Drives the source-list error overlay; called
  // only when the batch fields changed, not every tick.
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

  static constexpr auto kPreviewRefresh = std::chrono::milliseconds(250);  // between two preview recipe installs
  static constexpr auto kTrialPeriod = std::chrono::milliseconds(500);     // between two trials: the readout at 2 Hz
  static constexpr auto kSeriesRead = std::chrono::milliseconds(500);      // between two reads of the preview series
  static constexpr std::string_view kPreviewId = "__te_preview__";
  static constexpr std::string_view kObjectPreviewId = "__te_obj_preview__";

  TransformEditorDialog dialog_;
  bool callbacks_wired_ = false;
  PJ::sdk::DataProcessorsHostView dp_view_;
  PJ::sdk::PlaybackHostView playback_view_;
  PJ::sdk::PlotTabHostView plot_tabs_view_;
  PJ::sdk::ToolboxHostView test_catalog_host_{PJ_toolbox_host_t{}};  // set by tests only; see catalogHost()
  static constexpr int kCatalogRefreshTicks = 20;
  int catalog_refresh_ticks_ = 0;                         // 0 = never read yet
  std::optional<PJ::sdk::CatalogSnapshotV2> catalog_v2_;  // empty when the host cannot do on-demand
  std::string catalog_v2_error_;
  std::optional<std::set<std::string>> catalog_scalar_paths_;  // set only on a host without catalog snapshot v2
  struct Validation {  // the host's verdict on the script of one form revision (see scriptError)
    std::uint64_t revision = 0;
    std::string kind;
    std::optional<std::string> error;
  };
  std::optional<Validation> validation_;
  std::optional<OnDemandBuild> on_demand_build_;  // see buildOnDemandRequest()
  std::uint64_t on_demand_build_revision_ = 0;    // the form revision it was built at
  int on_demand_build_count_ = 0;                 // rebuilds so far (tests assert the cache holds)
  // "Show in 3D/2D" for the recipe just created: the tab id, its kind, and (topic, dataset) to attach.
  static constexpr std::string_view kSceneIdPrefix = "te_";
  std::string scene_id_;
  std::string scene_kind_;
  std::vector<PJ::SceneTopic> scene_topics_;
  std::optional<std::uint64_t> pending_preview_;
  std::chrono::steady_clock::time_point preview_deadline_;
  std::chrono::steady_clock::time_point next_preview_refresh_;
  std::chrono::steady_clock::time_point next_object_preview_refresh_;  // debounce of the object preview
  // The trial (see previewOnDemand): what the form asked for last (without the instant), when it changed,
  // whether a run is owed to it, and the cursor instant that had no sample.
  std::string trial_form_key_;
  std::chrono::steady_clock::time_point trial_edit_at_;
  bool trial_form_pending_ = false;
  std::chrono::milliseconds edit_debounce_{300};  // quiet time after the last edit before a trial runs
  std::optional<std::int64_t> no_sample_instant_;
  bool showing_on_demand_ = false;  // the last refresh was a recipe evaluated at the cursor
  std::chrono::steady_clock::time_point next_series_read_;
  bool series_available_ = false;                   // the host materialized a series for a number output
  std::vector<std::string> object_preview_topics_;  // the topics the host resolved for the preview recipe's outputs
  std::string preview_key_;                         // non-empty when an ephemeral preview node is live
  // The preview of object outputs (see updatePreviewRecipe).
  bool object_preview_live_ = false;      // the ephemeral on_demand recipe is installed
  std::string object_preview_signature_;  // what it was built from (also set when the host refused it)
  // The (form revision, trial serial) the recipe above was last reconciled with, and what that left: the
  // outputs it declares and whether one is an object.
  std::pair<std::uint64_t, std::uint64_t> preview_recipe_stamp_{};
  std::vector<PJ::sdk::DataProcessorOutput> preview_recipe_outputs_;
  std::uint64_t object_preview_installs_ = 0;  // how many times the host accepted the recipe
  // What the preview series were last read at: the install count and the object inputs' data stamp.
  struct SeriesStamp {
    std::uint64_t installs = 0;
    std::vector<std::pair<std::uint64_t, std::int64_t>> data;
    bool operator==(const SeriesStamp&) const = default;
  };
  SeriesStamp series_read_stamp_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(TransformEditorToolbox, kTransformEditorManifest)
PJ_DIALOG_PLUGIN(TransformEditorDialog, kTransformEditorManifest)
