#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "quaternion_manifest.hpp"
#include "quaternion_dialog_ui.hpp"
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

  std::string ui_content() const override { return kQuaternionDialogUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setItems("input_x", available_fields_)
        .setItems("input_y", available_fields_)
        .setItems("input_z", available_fields_)
        .setItems("input_w", available_fields_);

    // Restore selection if previously set.
    auto select = [&](const char* name, const std::string& value) {
      auto it = std::find(available_fields_.begin(), available_fields_.end(), value);
      if (it != available_fields_.end()) {
        wd.setCurrentIndex(name, static_cast<int>(it - available_fields_.begin()));
      }
    };
    select("input_x", input_x_);
    select("input_y", input_y_);
    select("input_z", input_z_);
    select("input_w", input_w_);

    wd.setText("output_prefix", output_prefix_)
        .setChecked("unwrap_check", unwrap_)
        .setChecked("degrees_check", degrees_)
        .setText("status_label", status_msg_)
        .setOkEnabled(isValid());
    return wd.toJson();
  }

  bool onIndexChanged(std::string_view name, int index) override {
    if (index < 0 || static_cast<size_t>(index) >= available_fields_.size()) return false;
    const auto& value = available_fields_[static_cast<size_t>(index)];
    if (name == "input_x") { input_x_ = value; }
    else if (name == "input_y") { input_y_ = value; }
    else if (name == "input_z") { input_z_ = value; }
    else if (name == "input_w") { input_w_ = value; }
    else { return false; }
    status_msg_.clear();
    return true;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "output_prefix") { output_prefix_ = std::string(text); return true; }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "unwrap_check") { unwrap_ = checked; return true; }
    if (name == "degrees_check") { degrees_ = checked; return true; }
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
    if (j.is_discarded()) return false;
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
    return !input_x_.empty() && !input_y_.empty() &&
           !input_z_.empty() && !input_w_.empty() &&
           !output_prefix_.empty();
  }

  void setAvailableFields(std::vector<std::string> fields) {
    available_fields_ = std::move(fields);
    // Auto-select first available field for any empty input.
    if (!available_fields_.empty()) {
      if (input_x_.empty()) input_x_ = available_fields_.front();
      if (input_y_.empty()) input_y_ = available_fields_.front();
      if (input_z_.empty()) input_z_ = available_fields_.front();
      if (input_w_.empty()) input_w_ = available_fields_.front();
    }
  }

  [[nodiscard]] const std::string& inputX() const { return input_x_; }
  [[nodiscard]] const std::string& inputY() const { return input_y_; }
  [[nodiscard]] const std::string& inputZ() const { return input_z_; }
  [[nodiscard]] const std::string& inputW() const { return input_w_; }
  [[nodiscard]] const std::string& outputPrefix() const { return output_prefix_; }
  [[nodiscard]] bool unwrap() const { return unwrap_; }
  [[nodiscard]] bool degrees() const { return degrees_; }

  void setStatus(std::string msg) { status_msg_ = std::move(msg); }

 private:
  std::string input_x_;
  std::string input_y_;
  std::string input_z_;
  std::string input_w_;
  std::string output_prefix_ = "rpy/";
  bool unwrap_ = true;
  bool degrees_ = true;
  std::string status_msg_;
  std::vector<std::string> available_fields_;
};

// ---------------------------------------------------------------------------
// QuaternionToolbox
// ---------------------------------------------------------------------------

class QuaternionToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  void* dialogContext() override {
    // Populate available fields from catalog before the dialog opens.
    if (toolboxHostBound()) {
      auto host = toolboxHost();
      auto catalog = host.catalogSnapshot();
      if (catalog) {
        std::vector<std::string> names;
        for (const auto& f : catalog->fields()) {
          names.emplace_back(PJ::sdk::toStringView(f.name));
        }
        std::sort(names.begin(), names.end());
        dialog_.setAvailableFields(std::move(names));
      }
    }
    return &dialog_;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("invalid config JSON");
    }
    if (dialog_.isValid() && toolboxHostBound() && runtimeHostBound()) {
      return applyTransform();
    }
    return PJ::okStatus();
  }

 private:
  PJ::Status applyTransform() {
    auto host = toolboxHost();

    // 1. Discover fields from catalog.
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return PJ::unexpected("failed to acquire catalog: " + std::string(catalog.error()));
    }

    auto find_field = [&](const std::string& name)
        -> PJ::Expected<PJ::sdk::FieldHandle> {
      for (const auto& f : catalog->fields()) {
        if (PJ::sdk::toStringView(f.name) == name) {
          return f.handle;
        }
      }
      return PJ::unexpected("field not found: " + name);
    };

    auto field_x = find_field(dialog_.inputX());
    auto field_y = find_field(dialog_.inputY());
    auto field_z = find_field(dialog_.inputZ());
    auto field_w = find_field(dialog_.inputW());

    if (!field_x || !field_y || !field_z || !field_w) {
      std::string err = "Missing input fields:";
      if (!field_x) err += " X(" + dialog_.inputX() + ")";
      if (!field_y) err += " Y(" + dialog_.inputY() + ")";
      if (!field_z) err += " Z(" + dialog_.inputZ() + ")";
      if (!field_w) err += " W(" + dialog_.inputW() + ")";
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
    if (series_y->timestamps().size() != count ||
        series_z->timestamps().size() != count ||
        series_w->timestamps().size() != count) {
      return PJ::unexpected("input series have different lengths");
    }

    // 3. Compute RPY.
    QuaternionToRPYConverter converter;
    converter.setScale(dialog_.degrees() ? kDegPerRad : 1.0);
    converter.setUnwrap(dialog_.unwrap());
    converter.reset();

    // Access raw double values from MaterializedSeries.
    const auto& raw_x = series_x->raw();
    const auto& raw_y = series_y->raw();
    const auto& raw_z = series_z->raw();
    const auto& raw_w = series_w->raw();

    auto as_double = [](const PJ_materialized_series_t& s, size_t i) -> double {
      return s.values.as_float64[i];
    };

    // 4. Write output.
    auto source = host.createDataSource("quaternion_rpy");
    if (!source) {
      return PJ::unexpected("failed to create output data source");
    }

    std::string prefix = dialog_.outputPrefix();
    auto topic = host.ensureTopic(*source, prefix + "rpy");
    if (!topic) {
      return PJ::unexpected("failed to create output topic");
    }

    for (size_t i = 0; i < count; ++i) {
      std::array<double, 4> quat = {
          as_double(raw_x, i),
          as_double(raw_y, i),
          as_double(raw_z, i),
          as_double(raw_w, i)};

      std::array<double, 3> rpy{};
      converter.convert(i, quat, rpy);

      const PJ::sdk::NamedFieldValue fields[] = {
          {.name = "roll", .value = rpy[0]},
          {.name = "pitch", .value = rpy[1]},
          {.name = "yaw", .value = rpy[2]},
      };

      auto status = host.appendRecord(*topic, ts[i], PJ::Span(fields));
      if (!status) {
        return PJ::unexpected("failed to append record at index " + std::to_string(i));
      }
    }

    runtimeHost().notifyDataChanged();
    dialog_.setStatus("Converted " + std::to_string(count) + " samples");
    return PJ::okStatus();
  }

  QuaternionDialog dialog_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(QuaternionToolbox, kQuaternionManifest)
PJ_DIALOG_PLUGIN(QuaternionDialog)
