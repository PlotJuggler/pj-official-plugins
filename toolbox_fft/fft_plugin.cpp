#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fft_manifest.hpp"
#include "fft_dialog_ui.hpp"

extern "C" {
#include <kissfft/kiss_fftr.h>
}

// ---------------------------------------------------------------------------
// FFT math (ported from PJ 3.x ToolboxFFT / proto_app fft_editor.cpp)
// ---------------------------------------------------------------------------

namespace {

struct FftResult {
  std::vector<double> frequencies_hz;
  std::vector<double> amplitudes;
};

std::optional<FftResult> computeFFT(const int64_t* timestamps, const double* values, size_t count,
                                    bool remove_dc) {
  if (count < 8) return std::nullopt;

  size_t n = count;
  if ((n & 1U) != 0U) --n;  // make even

  double dt_seconds =
      static_cast<double>(timestamps[n - 1] - timestamps[0]) / (static_cast<double>(n - 1) * 1e9);
  if (dt_seconds <= 0.0) return std::nullopt;

  std::vector<kiss_fft_scalar> input(n);
  double average = 0.0;
  if (remove_dc) {
    for (size_t i = 0; i < n; ++i) {
      average += values[i];
    }
    average /= static_cast<double>(n);
  }

  for (size_t i = 0; i < n; ++i) {
    input[i] = static_cast<kiss_fft_scalar>(values[i] - average);
  }

  std::vector<kiss_fft_cpx> out(n / 2 + 1);
  kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
  if (cfg == nullptr) return std::nullopt;

  kiss_fftr(cfg, input.data(), out.data());
  KISS_FFT_FREE(cfg);

  FftResult res;
  res.frequencies_hz.reserve(n / 2);
  res.amplitudes.reserve(n / 2);

  const double nd = static_cast<double>(n);
  for (size_t i = 0; i < n / 2; ++i) {
    res.frequencies_hz.push_back(static_cast<double>(i) * (1.0 / dt_seconds) / nd);
    res.amplitudes.push_back(
        std::hypot(static_cast<double>(out[i].r), static_cast<double>(out[i].i)) / nd);
  }

  return res;
}

// ---------------------------------------------------------------------------
// FFTDialog
// ---------------------------------------------------------------------------

class FFTDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override { return kFftManifest; }

  std::string ui_content() const override { return kFftDialogUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setListItems("field_list", available_fields_)
        .setSelectedItems("field_list", selected_fields_)
        .setChecked("check_dc_removal", remove_dc_)
        .setText("suffix_edit", suffix_)
        .setEnabled("btn_compute", !selected_fields_.empty())
        .setText("status_label", status_msg_);
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name,
                          const std::vector<std::string>& selected) override {
    if (name == "field_list") {
      selected_fields_ = selected;
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "check_dc_removal") {
      remove_dc_ = checked;
      return false;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "suffix_edit") {
      suffix_ = std::string(text);
      return false;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "btn_compute") {
      compute_requested_ = true;
      return true;
    }
    return false;
  }

  std::string saveConfig() const override {
    nlohmann::json j;
    j["remove_dc"] = remove_dc_;
    j["suffix"] = suffix_;
    j["selected_fields"] = selected_fields_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) return false;

    remove_dc_ = j.value("remove_dc", false);
    suffix_ = j.value("suffix", std::string{"_FFT"});

    saved_selected_.clear();
    if (j.contains("selected_fields") && j["selected_fields"].is_array()) {
      for (const auto& item : j["selected_fields"]) {
        if (item.is_string()) {
          saved_selected_.push_back(item.get<std::string>());
        }
      }
    }
    return true;
  }

  void setAvailableFields(const std::vector<std::string>& fields) {
    if (fields == available_fields_) return;

    available_fields_ = fields;

    std::vector<std::string> merged = selected_fields_;
    for (const auto& saved : saved_selected_) {
      if (std::find(merged.begin(), merged.end(), saved) == merged.end()) {
        merged.push_back(saved);
      }
    }

    selected_fields_.clear();
    for (const auto& sel : merged) {
      if (std::find(available_fields_.begin(), available_fields_.end(), sel) !=
          available_fields_.end()) {
        selected_fields_.push_back(sel);
      }
    }
  }

  void setStatus(const std::string& msg) { status_msg_ = msg; }

  [[nodiscard]] const std::vector<std::string>& selectedFields() const { return selected_fields_; }
  [[nodiscard]] bool removeDC() const { return remove_dc_; }
  [[nodiscard]] const std::string& suffix() const { return suffix_; }

  [[nodiscard]] bool consumeComputeRequest() {
    const bool req = compute_requested_;
    compute_requested_ = false;
    return req;
  }

 private:
  std::vector<std::string> available_fields_;
  std::vector<std::string> selected_fields_;
  std::vector<std::string> saved_selected_;
  bool remove_dc_ = false;
  std::string suffix_ = "_FFT";
  std::string status_msg_;
  bool compute_requested_ = false;
};

// ---------------------------------------------------------------------------
// FFTToolbox
// ---------------------------------------------------------------------------

class FFTToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  void* dialogContext() override {
    refreshFieldList();
    if (dialog_.consumeComputeRequest()) {
      runComputation();
    }
    return &dialog_;
  }

  PJ::Status bindToolboxHost(PJ_toolbox_host_t host) override {
    auto status = ToolboxPluginBase::bindToolboxHost(host);
    if (!status) return status;

    refreshFieldList();
    return PJ::okStatus();
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

 private:
  void refreshFieldList() {
    if (!toolboxHostBound()) return;

    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) return;

    std::vector<std::string> fields;
    field_index_.clear();

    auto all_topics = catalog->topics();
    auto all_fields = catalog->fields();

    for (size_t ti = 0; ti < all_topics.size(); ++ti) {
      const auto& topic = all_topics[ti];
      auto topic_name = PJ::sdk::toStringView(topic.name);

      for (uint32_t fi = 0; fi < topic.field_count; ++fi) {
        const auto& field = all_fields[topic.first_field + fi];
        auto field_name = PJ::sdk::toStringView(field.name);

        std::string path = std::string(topic_name) + "/" + std::string(field_name);
        field_index_[path] = field.handle;
        fields.push_back(path);
      }
    }

    std::sort(fields.begin(), fields.end());
    dialog_.setAvailableFields(fields);
  }

  void runComputation() {
    if (!toolboxHostBound()) {
      dialog_.setStatus("Error: toolbox host not bound");
      return;
    }

    const auto& selected = dialog_.selectedFields();
    if (selected.empty()) {
      dialog_.setStatus("No fields selected");
      return;
    }

    auto host = toolboxHost();
    const std::string& suffix = dialog_.suffix();
    const bool remove_dc = dialog_.removeDC();

    int computed = 0;
    int skipped = 0;

    for (const auto& field_path : selected) {
      auto it = field_index_.find(field_path);
      if (it == field_index_.end()) {
        ++skipped;
        continue;
      }

      auto series = host.readSeries(it->second);
      if (!series) {
        ++skipped;
        continue;
      }

      if (series->type() != PJ::PrimitiveType::kFloat64) {
        ++skipped;
        continue;
      }

      auto timestamps = series->timestamps();
      const size_t row_count = series->raw().row_count;
      const double* values = series->raw().values.as_float64;

      auto result = computeFFT(timestamps.data(), values, row_count, remove_dc);
      if (!result) {
        ++skipped;
        continue;
      }

      auto source = host.createDataSource("fft_output");
      if (!source) {
        ++skipped;
        continue;
      }

      std::string output_name = field_path + suffix;
      auto topic = host.ensureTopic(*source, output_name);
      if (!topic) {
        ++skipped;
        continue;
      }

      for (size_t i = 0; i < result->frequencies_hz.size(); ++i) {
        auto ts = PJ::Timestamp{static_cast<int64_t>(result->frequencies_hz[i] * 1e9)};
        const PJ::sdk::NamedFieldValue nfv[] = {
            {.name = "amplitude", .value = result->amplitudes[i]},
        };
        auto append_status = host.appendRecord(*topic, ts, PJ::Span(nfv));
        (void)append_status;
      }

      ++computed;
    }

    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }

    std::string msg = "Computed FFT for " + std::to_string(computed) + " field(s)";
    if (skipped > 0) {
      msg += ", skipped " + std::to_string(skipped);
    }
    dialog_.setStatus(msg);
  }

  FFTDialog dialog_;
  std::map<std::string, PJ::sdk::FieldHandle> field_index_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(FFTToolbox, kFftManifest)
PJ_DIALOG_PLUGIN(FFTDialog)