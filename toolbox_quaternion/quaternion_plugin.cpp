#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quaternion_dialog_ui.hpp"
#include "quaternion_manifest.hpp"
#include "quaternion_to_rpy.hpp"

namespace {

constexpr double kDegPerRad = QuaternionToRPYConverter::kDegPerRad;

class QuaternionDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kQuaternionManifest;  // embedded manifest.json — carries the required "id"
  }

  std::string ui_content() const override {
    return kQuaternionDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setDropTarget("inputFrame");
    wd.setText("input_x", input_x_);
    wd.setText("input_y", input_y_);
    wd.setText("input_z", input_z_);
    wd.setText("input_w", input_w_);

    wd.setText("output_prefix", output_prefix_)
        .setChecked("unwrap_check", unwrap_)
        .setChecked("radio_degrees", degrees_)
        .setChecked("radio_radians", !degrees_)
        .setText("status_label", status_msg_)
        .setEnabled("save_button", isValid());

    wd.setChartPlaceholder("chart_preview", "Drag and Drop Quaternion values in the frames above.");

    if (isValid()) {
      auto preview = computePreview();
      if (!preview.empty()) {
        wd.setChartSeries("chart_preview", preview);
      } else {
        wd.clearChart("chart_preview");
      }
    } else {
      wd.clearChart("chart_preview");
    }

    return wd.toJson();
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "output_prefix") {
      output_prefix_ = std::string(text);
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "unwrap_check") {
      unwrap_ = checked;
      return true;
    }
    if (name == "radio_degrees") {
      degrees_ = checked;
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "save_button" && isValid()) {
      // The non-modal PanelEngine ignores requestAccept(), so apply through the toolbox's callback (it owns the
      // host write surface).
      if (on_save_) {
        on_save_();
      }
      return true;
    }
    return false;
  }

  bool onItemsDropped(std::string_view /*name*/, const std::vector<std::string>& items) override {
    if (items.empty()) {
      return false;
    }

    const auto& dropped = items.front();
    auto last_slash = dropped.rfind('/');
    if (last_slash == std::string::npos) {
      return false;
    }

    std::string prefix = dropped.substr(0, last_slash + 1);
    std::string suffix = dropped.substr(last_slash + 1);

    static constexpr std::array<std::array<const char*, 4>, 2> kPatterns = {{
        {{"x", "y", "z", "w"}},
        {{"qx", "qy", "qz", "qw"}},
    }};

    auto field_exists = [&](const std::string& field) {
      return std::find(available_fields_.begin(), available_fields_.end(), field) != available_fields_.end();
    };

    for (const auto& pattern : kPatterns) {
      bool suffix_matches = false;
      for (const auto* p : pattern) {
        if (suffix == p) {
          suffix_matches = true;
          break;
        }
      }
      if (!suffix_matches) {
        continue;
      }

      std::string fx = prefix + pattern[0];
      std::string fy = prefix + pattern[1];
      std::string fz = prefix + pattern[2];
      std::string fw = prefix + pattern[3];

      // available_fields_ is filled only in getDialog() and onDataChanged() is not invoked on file load, so a
      // toolbox opened before any data was loaded has an empty list and would reject every drop. Validate the four
      // siblings only when the list is populated; otherwise accept the best-effort match (applyTransform()
      // re-checks against the live catalog on Save).
      const bool list_known = !available_fields_.empty();
      if (!list_known || (field_exists(fx) && field_exists(fy) && field_exists(fz) && field_exists(fw))) {
        input_x_ = fx;
        input_y_ = fy;
        input_z_ = fz;
        input_w_ = fw;
        output_prefix_ = prefix + "rpy/";
        status_msg_.clear();
        // The data snapshot is taken in getDialog() and may be empty; the drop is when the preview's series become
        // known, so re-read them now. No periodic polling.
        if (on_refresh_) {
          on_refresh_();
        }
        return true;
      }
    }

    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json j;
    j["input_x"] = input_x_;
    j["input_y"] = input_y_;
    j["input_z"] = input_z_;
    j["input_w"] = input_w_;
    j["output_prefix"] = output_prefix_;
    j["unwrap"] = unwrap_;
    j["degrees"] = degrees_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) {
      return false;
    }
    input_x_ = j.value("input_x", std::string{});
    input_y_ = j.value("input_y", std::string{});
    input_z_ = j.value("input_z", std::string{});
    input_w_ = j.value("input_w", std::string{});
    output_prefix_ = j.value("output_prefix", std::string{});
    unwrap_ = j.value("unwrap", true);
    degrees_ = j.value("degrees", true);
    status_msg_.clear();
    return true;
  }

  [[nodiscard]] bool isValid() const {
    return !input_x_.empty() && !input_y_.empty() && !input_z_.empty() && !input_w_.empty() && !output_prefix_.empty();
  }

  void setAvailableFields(std::vector<std::string> fields) {
    available_fields_ = std::move(fields);
  }

  [[nodiscard]] const std::string& inputX() const {
    return input_x_;
  }
  [[nodiscard]] const std::string& inputY() const {
    return input_y_;
  }
  [[nodiscard]] const std::string& inputZ() const {
    return input_z_;
  }
  [[nodiscard]] const std::string& inputW() const {
    return input_w_;
  }
  [[nodiscard]] const std::string& outputPrefix() const {
    return output_prefix_;
  }
  [[nodiscard]] bool unwrap() const {
    return unwrap_;
  }
  [[nodiscard]] bool degrees() const {
    return degrees_;
  }

  void setStatus(std::string msg) {
    status_msg_ = std::move(msg);
  }

  struct SeriesData {
    std::vector<double> timestamps;
    std::vector<double> values;
  };

  void setSeriesDataMap(std::unordered_map<std::string, SeriesData> data) {
    series_data_ = std::move(data);
  }

  // Set by the toolbox, which owns the host write surface the dialog lacks.
  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }

  // Set by the toolbox (the dialog has no catalog access): re-reads the field list and preview data on drop.
  void setOnRefresh(std::function<void()> cb) {
    on_refresh_ = std::move(cb);
  }

 private:
  std::vector<PJ::ChartSeries> computePreview() const {
    auto find = [&](const std::string& name) -> const SeriesData* {
      auto it = series_data_.find(name);
      return (it != series_data_.end()) ? &it->second : nullptr;
    };

    const auto* sx = find(input_x_);
    const auto* sy = find(input_y_);
    const auto* sz = find(input_z_);
    const auto* sw = find(input_w_);
    if (!sx || !sy || !sz || !sw) {
      return {};
    }

    size_t count = sx->timestamps.size();
    if (count == 0 || sy->values.size() != count || sz->values.size() != count || sw->values.size() != count) {
      return {};
    }

    QuaternionToRPYConverter converter;
    converter.setScale(degrees_ ? kDegPerRad : 1.0);
    converter.setUnwrap(unwrap_);
    converter.reset();

    PJ::ChartSeries roll_s{"roll", {}, {}};
    PJ::ChartSeries pitch_s{"pitch", {}, {}};
    PJ::ChartSeries yaw_s{"yaw", {}, {}};
    roll_s.points.reserve(count);
    pitch_s.points.reserve(count);
    yaw_s.points.reserve(count);

    // X axis: seconds from the first sample (timestamps are ns).
    double t0 = sx->timestamps.front();

    for (size_t i = 0; i < count; ++i) {
      std::array<double, 4> quat = {sx->values[i], sy->values[i], sz->values[i], sw->values[i]};
      std::array<double, 3> rpy{};
      converter.convert(i, quat, rpy);

      double t = (sx->timestamps[i] - t0) / 1e9;
      roll_s.points.push_back({t, rpy[0]});
      pitch_s.points.push_back({t, rpy[1]});
      yaw_s.points.push_back({t, rpy[2]});
    }

    return {std::move(roll_s), std::move(pitch_s), std::move(yaw_s)};
  }

  std::string input_x_;
  std::string input_y_;
  std::string input_z_;
  std::string input_w_;
  std::string output_prefix_ = "rpy/";
  bool unwrap_ = true;
  bool degrees_ = true;
  std::function<void()> on_save_;
  std::function<void()> on_refresh_;
  std::string status_msg_;
  std::vector<std::string> available_fields_;
  std::unordered_map<std::string, SeriesData> series_data_;
};

class QuaternionToolbox : public PJ::ToolboxPluginBase {
 public:
  QuaternionToolbox() {
    // Non-modal toolbox panels don't round-trip accept()/loadConfig() on Save, so Save installs the transform here.
    dialog_.setOnSave([this]() {
      if (!dialog_.isValid()) {
        return;
      }
      created_ = false;  // an explicit Save always rebuilds with the current options
      if (auto status = applyTransform(); !status) {
        reportWarning("quaternion save failed: " + std::string(status.error()));
      }
    });

    // A toolbox opened before the data loads snapshots an empty catalog and there is no toolbox tick, so re-read
    // the catalog on each drop (when the needed series become known).
    dialog_.setOnRefresh([this]() { refreshDialogFromCatalog(); });
  }

  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    // The host runs the plugin's Luau class (4 inputs -> 3 outputs) live as a DerivedEngine node. Optional: an
    // older host does not expose the service.
    if (auto dp = services.get<PJ::sdk::DataProcessorsHostService>()) {
      dp_view_ = *dp;
    }
    return PJ::okStatus();
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Snapshot the catalog before opening; later refreshes happen on drop (see ctor).
    refreshDialogFromCatalog();
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto prev_config = dialog_.saveConfig();
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("invalid config JSON");
    }
    if (dialog_.saveConfig() != prev_config) {
      created_ = false;  // options changed -> the node must be rebuilt
    }
    if (dialog_.isValid() && dp_view_.valid()) {
      // Best-effort: inputs may not resolve yet (layout restored before its data loaded). That is not a
      // config-load failure: onDataChanged reinstalls once the data arrives (created_ stays false on failure).
      (void)applyTransform();
    }
    return PJ::okStatus();
  }

  void onDataChanged() override {
    if (!dialog_.isValid() || !dp_view_.valid()) {
      return;
    }
    // The installed node recomputes live. Two cases still need a (re)install: the config arrived before the data,
    // and a session clear / dataset reload tore the node down while created_ stayed set (detected via the host's
    // live processor list).
    if (created_ && nodeStillInstalled()) {
      return;
    }
    if (auto status = applyTransform(); !status) {
      reportWarning("quaternion apply failed: " + std::string(status.error()));
    }
  }

 private:
  // list() is optional in the ABI; without it a teardown is unobservable, so assume the node persists rather than
  // rebuilding the script on every data change.
  [[nodiscard]] bool nodeStillInstalled() const {
    if (last_id_.empty()) {
      return false;
    }
    auto ids = dp_view_.list();
    if (!ids) {
      return true;
    }
    return std::find(ids->begin(), ids->end(), last_id_) != ids->end();
  }

  // Pulls the field names drops are validated against and the series the preview plots. Called on dialog open and
  // on each drop, so a toolbox opened before the data loaded still gets fresh series.
  void refreshDialogFromCatalog() {
    if (!toolboxHostBound()) {
      return;
    }
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return;
    }

    std::vector<std::string> names;
    std::unordered_map<std::string, QuaternionDialog::SeriesData> data_map;
    auto all_fields = catalog->fields();
    for (const auto& topic : catalog->topics()) {
      std::string topic_name(PJ::sdk::toStringView(topic.name));
      for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
        const auto& f = all_fields[fi];
        std::string name = topic_name + "/" + std::string(PJ::sdk::toStringView(f.name));
        names.push_back(name);

        auto series = host.readSeries(f.handle);
        if (series && series->type() == PJ::PrimitiveType::kFloat64) {
          auto ts = series->timestamps();
          const double* values = series->valuesAsFloat64();
          size_t count = ts.size();

          if (values != nullptr) {
            QuaternionDialog::SeriesData sd;
            sd.timestamps.resize(count);
            sd.values.resize(count);
            for (size_t i = 0; i < count; ++i) {
              sd.timestamps[i] = static_cast<double>(ts[i]);
              sd.values[i] = values[i];
            }
            data_map[name] = std::move(sd);
          }
        }
      }
    }

    std::sort(names.begin(), names.end());
    dialog_.setAvailableFields(std::move(names));
    dialog_.setSeriesDataMap(std::move(data_map));
  }

  PJ::Status applyTransform() {
    if (!dp_view_.valid()) {
      return PJ::unexpected("host did not expose pj.data_processors.v1");
    }

    // Inputs are FIELD names ("quat/w", ...) the host binds to topic columns. Order is w, x, y, z so the script's
    // calculate(time, value, v1, v2, v3) reads value=w, v1=x, v2=y, v3=z.
    const std::array<std::string, 4> input_names = {
        dialog_.inputW(), dialog_.inputX(), dialog_.inputY(), dialog_.inputZ()};

    // ONE topic with roll/pitch/yaw columns, as a grouped-output entry "<topic>:f1,f2,...": the prefix without its
    // trailing '/' is the topic.
    std::string topic = dialog_.outputPrefix();
    while (!topic.empty() && topic.back() == '/') {
      topic.pop_back();
    }
    // ':' separates topic from fields and ',' separates fields in the grouped-output
    // spec; a topic containing either would corrupt the host's parse, so neutralize
    // them (the prefix is free-form user text).
    std::replace(topic.begin(), topic.end(), ':', '_');
    std::replace(topic.begin(), topic.end(), ',', '_');
    if (topic.empty()) {
      topic = "rpy";
    }
    const std::string id = topic;  // host namespaces it under the plugin id
    const std::string grouped_output = topic + ":roll,pitch,yaw";

    // An edited output prefix changes the id: drop the old node so its output does not linger.
    if (!last_id_.empty() && last_id_ != id) {
      (void)dp_view_.remove(last_id_);
    }

    const std::string script = buildScript(id, dialog_.degrees(), dialog_.unwrap());
    const std::vector<std::string_view> inputs(input_names.begin(), input_names.end());
    const std::vector<std::string_view> outputs = {grouped_output};

    auto status = dp_view_.createTransform(
        id, PJ::Span<const std::string_view>(inputs.data(), inputs.size()),
        PJ::Span<const std::string_view>(outputs.data(), outputs.size()), script, "{}");
    if (!status) {
      dialog_.setStatus("Create failed: " + std::string(status.error()));
      return status;
    }

    last_id_ = id;
    created_ = true;
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    dialog_.setStatus("Created roll/pitch/yaw transform.");
    return PJ::okStatus();
  }

  void reportWarning(const std::string& message) {
    if (runtimeHostBound()) {
      runtimeHost().reportMessage(PJ::ToolboxMessageLevel::kWarning, message);
    }
  }

  // Luau class mirroring QuaternionToRPYConverter. The unwrap state lives in the factory closure and relies on the
  // engine calling calculate() in ascending timestamp order. `degrees` bakes the rad->deg scale into the script.
  static std::string buildScript(const std::string& id, bool degrees, bool unwrap) {
    std::string s;
    s += "-- pj-script: luau\n";
    s += "local function _pj_make()\n";
    s += "  local SCALE = ";
    s += degrees ? "180.0 / math.pi" : "1.0";
    s += "\n";
    s += "  local UNWRAP = ";
    s += unwrap ? "true" : "false";
    s += "\n";
    s += "  local WRAP = 2.0 * math.pi\n";
    s += "  local THRESH = 1.95 * math.pi\n";
    s += "  local prev = { 0.0, 0.0, 0.0 }\n";
    s += "  local off = { 0.0, 0.0, 0.0 }\n";
    s += "  local first = true\n";
    s += "  local function unwrap_angle(cur, i)\n";
    s += "    if (not first) and UNWRAP then\n";
    s += "      if (cur - prev[i]) > THRESH then\n";
    s += "        off[i] = off[i] - WRAP\n";
    s += "      elseif (prev[i] - cur) > THRESH then\n";
    s += "        off[i] = off[i] + WRAP\n";
    s += "      end\n";
    s += "    end\n";
    s += "    prev[i] = cur\n";
    s += "    return cur + off[i]\n";
    s += "  end\n";
    s += "  return function(time, value, v1, v2, v3)\n";
    s += "    local qw, qx, qy, qz = value, v1, v2, v3\n";
    s += "    local norm2 = qw*qw + qx*qx + qy*qy + qz*qz\n";
    s += "    if math.abs(norm2 - 1.0) > 1e-12 then\n";
    s += "      local inv = 1.0 / math.sqrt(norm2)\n";
    s += "      qx, qy, qz, qw = qx*inv, qy*inv, qz*inv, qw*inv\n";
    s += "    end\n";
    s += "    local sinr = 2.0 * (qw*qx + qy*qz)\n";
    s += "    local cosr = 1.0 - 2.0 * (qx*qx + qy*qy)\n";
    s += "    local roll = math.atan2(sinr, cosr)\n";
    s += "    local sinp = 2.0 * (qw*qy - qz*qx)\n";
    s += "    local pitch\n";
    s += "    if math.abs(sinp) >= 1.0 then\n";
    s += "      pitch = (sinp >= 0.0 and 1.0 or -1.0) * (math.pi / 2.0)\n";
    s += "    else\n";
    s += "      pitch = math.asin(sinp)\n";
    s += "    end\n";
    s += "    local siny = 2.0 * (qw*qz + qx*qy)\n";
    s += "    local cosy = 1.0 - 2.0 * (qy*qy + qz*qz)\n";
    s += "    local yaw = math.atan2(siny, cosy)\n";
    s += "    local r = unwrap_angle(roll, 1)\n";
    s += "    local p = unwrap_angle(pitch, 2)\n";
    s += "    local y = unwrap_angle(yaw, 3)\n";
    s += "    first = false\n";
    s += "    return SCALE*r, SCALE*p, SCALE*y\n";
    s += "  end\n";
    s += "end\n";
    s += "local T = { id = \"" + id + "\", name = \"" + id + "\", output = \"double\" }\n";
    s += "T.__index = T\n";
    s += "function T.create(_) return setmetatable({ fn = _pj_make() }, T) end\n";
    s += "function T:calculate(t, v, ...) return self.fn(t, v, ...) end\n";
    s += "return T\n";
    return s;
  }

  QuaternionDialog dialog_;
  PJ::sdk::DataProcessorsHostView dp_view_;
  std::string last_id_;   // id of the currently installed node (for re-target/remove)
  bool created_ = false;  // a live host node currently exists for this config
};

}  // namespace

PJ_TOOLBOX_PLUGIN(QuaternionToolbox, kQuaternionManifest)
PJ_DIALOG_PLUGIN(QuaternionDialog, kQuaternionManifest)
