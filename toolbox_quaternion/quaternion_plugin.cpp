#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
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

// ---------------------------------------------------------------------------
// QuaternionDialog
// ---------------------------------------------------------------------------

namespace {

constexpr double kDegPerRad = QuaternionToRPYConverter::kDegPerRad;

class QuaternionDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return R"({"name":"Quaternion to RPY","version":"1.0.0"})";
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

    // Compute and attach preview chart if inputs are valid.
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
      // The toolbox hosts its dialog in a non-modal PanelEngine, which ignores
      // requestAccept(); so apply directly through the callback the toolbox
      // installs (it owns the host write surface this dialog cannot reach).
      if (on_save_) {
        on_save_();
      }
      return true;  // refresh widget_data so the status label / preview update
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

    // Try known quaternion component naming patterns.
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

      // available_fields_ is only filled when the dialog opens (getDialog), and
      // the toolbox's onDataChanged() is not invoked on file load, so a toolbox
      // opened BEFORE any data was loaded would have an empty list and reject
      // every drop. Validate the four siblings only when the list is populated;
      // otherwise accept the best-effort match — applyTransform() re-checks the
      // fields against the live catalog when the user clicks Save.
      const bool list_known = !available_fields_.empty();
      if (!list_known || (field_exists(fx) && field_exists(fy) && field_exists(fz) && field_exists(fw))) {
        input_x_ = fx;
        input_y_ = fy;
        input_z_ = fz;
        input_w_ = fw;
        output_prefix_ = prefix + "rpy/";
        status_msg_.clear();
        // The dialog's data snapshot is taken once in getDialog(); if the
        // toolbox was opened before the data was loaded it is stale/empty. The
        // drop is the moment we know which series the preview needs, so ask the
        // toolbox to (re-)read them from the catalog now. No periodic polling.
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

  // Installed by the owning toolbox: invoked when the user clicks Save so the
  // transform is applied (the dialog has no host write surface of its own).
  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }

  // Installed by the owning toolbox: invoked from onTick() so the toolbox can
  // refresh the field list / preview data from the catalog while the panel is
  // open (the dialog has no catalog access of its own).
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

    // Use relative time (seconds from first timestamp) for the X axis.
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

// ---------------------------------------------------------------------------
// QuaternionToolbox
// ---------------------------------------------------------------------------

class QuaternionToolbox : public PJ::ToolboxPluginBase {
 public:
  QuaternionToolbox() {
    // Wire the dialog's Save button to the transform. Non-modal toolbox panels
    // don't round-trip accept()/loadConfig() on Save, so this is the path that
    // actually materializes the RPY series when the user clicks Save.
    dialog_.setOnSave([this]() {
      if (!dialog_.isValid() || !toolboxHostBound() || !runtimeHostBound()) {
        return;
      }
      if (auto status = applyTransform(); !status) {
        runtimeHost().reportMessage(
            PJ::ToolboxMessageLevel::kWarning, "quaternion save failed: " + std::string(status.error()));
      }
    });

    // A toolbox opened *before* the data is loaded snapshots an empty catalog
    // in getDialog(), so its preview has no series to plot. There is no periodic
    // toolbox tick to lean on; instead, re-read the catalog when the user drops
    // curves (the moment the needed series become known). One fetch per drop.
    dialog_.setOnRefresh([this]() { refreshDialogFromCatalog(); });
  }

  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  void resetIncrementalState() {
    source_handle_ = std::nullopt;
    topic_handle_ = std::nullopt;
    processed_count_ = 0;
    converter_.reset();
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Populate the dialog's field list + preview data from the catalog before
    // opening; subsequent refreshes are driven by the dialog's onTick (see ctor).
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
      resetIncrementalState();
    }
    if (dialog_.isValid() && toolboxHostBound() && runtimeHostBound()) {
      return applyTransform();
    }
    return PJ::okStatus();
  }

  void onDataChanged() override {
    if (!toolboxHostBound() || !runtimeHostBound() || !dialog_.isValid()) {
      return;
    }
    auto status = applyTransform();
    if (!status) {
      runtimeHost().reportMessage(
          PJ::ToolboxMessageLevel::kWarning, "quaternion re-apply failed: " + std::string(status.error()));
    }
  }

 private:
  // Pull the catalog into the dialog: the field names it validates drops
  // against and the per-series data the preview chart plots. Called at two
  // discrete moments — when the dialog opens (getDialog) and when the user
  // drops curves (onItemsDropped via the on_refresh_ callback) — so a toolbox
  // opened before the data was loaded still gets fresh series for its preview.
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

        // Read series data for the preview chart.
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
    auto host = toolboxHost();

    // 1. Discover fields from catalog.
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return PJ::unexpected("failed to acquire catalog: " + std::string(catalog.error()));
    }

    auto find_field = [&](const std::string& full_name) -> PJ::Expected<PJ::sdk::FieldHandle> {
      auto all_fields = catalog->fields();
      for (const auto& topic : catalog->topics()) {
        std::string topic_name(PJ::sdk::toStringView(topic.name));
        for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
          const auto& f = all_fields[fi];
          std::string qualified = topic_name + "/" + std::string(PJ::sdk::toStringView(f.name));
          if (qualified == full_name) {
            return f.handle;
          }
        }
      }
      return PJ::unexpected("field not found: " + full_name);
    };

    auto field_x = find_field(dialog_.inputX());
    auto field_y = find_field(dialog_.inputY());
    auto field_z = find_field(dialog_.inputZ());
    auto field_w = find_field(dialog_.inputW());

    if (!field_x || !field_y || !field_z || !field_w) {
      std::string err = "Missing input fields:";
      if (!field_x) {
        err += " X(" + dialog_.inputX() + ")";
      }
      if (!field_y) {
        err += " Y(" + dialog_.inputY() + ")";
      }
      if (!field_z) {
        err += " Z(" + dialog_.inputZ() + ")";
      }
      if (!field_w) {
        err += " W(" + dialog_.inputW() + ")";
      }
      dialog_.setStatus(err);
      return PJ::unexpected(err);
    }

    // 2. Read input series.
    auto series_x = host.readSeries(*field_x);
    auto series_y = host.readSeries(*field_y);
    auto series_z = host.readSeries(*field_z);
    auto series_w = host.readSeries(*field_w);

    if (!series_x || !series_y || !series_z || !series_w) {
      return PJ::unexpected("failed to read one or more input series");
    }

    auto ts = series_x->timestamps();
    size_t count = ts.size();
    if (count == 0) {
      dialog_.setStatus("Input series are empty");
      return PJ::okStatus();
    }

    // Validate all series have the same length.
    if (series_y->timestamps().size() != count || series_z->timestamps().size() != count ||
        series_w->timestamps().size() != count) {
      return PJ::unexpected("input series have different lengths");
    }

    // 3. Configure converter (reuse state for incremental processing).
    converter_.setScale(dialog_.degrees() ? kDegPerRad : 1.0);
    converter_.setUnwrap(dialog_.unwrap());

    // Grab typed value pointers from the Arrow-backed series views.
    const double* vals_x = series_x->valuesAsFloat64();
    const double* vals_y = series_y->valuesAsFloat64();
    const double* vals_z = series_z->valuesAsFloat64();
    const double* vals_w = series_w->valuesAsFloat64();

    if (vals_x == nullptr || vals_y == nullptr || vals_z == nullptr || vals_w == nullptr) {
      return PJ::unexpected("input series must be float64");
    }

    // 4. Create data source and topic only on first invocation.
    if (!source_handle_) {
      auto source = host.createDataSource("quaternion_rpy");
      if (!source) {
        return PJ::unexpected("failed to create output data source");
      }
      source_handle_ = *source;
    }

    if (!topic_handle_) {
      std::string prefix = dialog_.outputPrefix();
      auto topic = host.ensureTopic(*source_handle_, prefix + "rpy");
      if (!topic) {
        return PJ::unexpected("failed to create output topic");
      }
      topic_handle_ = *topic;
    }

    // 5. Process only new points (incremental).
    size_t start = processed_count_;
    if (start >= count) {
      dialog_.setStatus("No new samples to process");
      return PJ::okStatus();
    }

    for (size_t i = start; i < count; ++i) {
      std::array<double, 4> quat = {vals_x[i], vals_y[i], vals_z[i], vals_w[i]};

      std::array<double, 3> rpy{};
      converter_.convert(i, quat, rpy);

      const PJ::sdk::NamedFieldValue fields[] = {
          {.name = "roll", .value = rpy[0]},
          {.name = "pitch", .value = rpy[1]},
          {.name = "yaw", .value = rpy[2]},
      };

      auto status = host.appendRecord(*topic_handle_, ts[i], PJ::Span<const PJ::sdk::NamedFieldValue>(fields));
      if (!status) {
        return PJ::unexpected("failed to append record at index " + std::to_string(i));
      }
    }

    processed_count_ = count;
    runtimeHost().notifyDataChanged();
    dialog_.setStatus("Converted " + std::to_string(count) + " samples (" + std::to_string(count - start) + " new)");
    return PJ::okStatus();
  }

  QuaternionDialog dialog_;

  // Incremental processing state — persists across applyTransform() calls.
  QuaternionToRPYConverter converter_;
  std::optional<PJ::sdk::DataSourceHandle> source_handle_;
  std::optional<PJ::sdk::TopicHandle> topic_handle_;
  size_t processed_count_ = 0;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(QuaternionToolbox, kQuaternionManifest)
PJ_DIALOG_PLUGIN(QuaternionDialog)
