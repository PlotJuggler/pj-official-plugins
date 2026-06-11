// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Filter Editor toolbox — PlotJuggler 3's "Apply filter to data" ported to a
// PJ4 toolbox panel. Pick a source curve, choose a built-in transform
// (Absolute, Scale, Derivative, Integral, Moving Average/RMS/Variance, Outlier
//
// Built-in math: transforms.hpp.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "builtin_transforms.hpp"
#include "filter_editor_dialog_ui.hpp"
#include "filter_editor_manifest.hpp"

namespace {

using PJ::sdk::BinaryFilterTransform;
using PJ::sdk::BinaryOp;
using PJ::sdk::DerivativeTransform;
using PJ::sdk::FilterTransform;
using PJ::sdk::FilterTransformFactory;
using PJ::sdk::IntegralTransform;
using PJ::sdk::MovingAverageTransform;
using PJ::sdk::MovingRMSTransform;
using PJ::sdk::MovingVarianceTransform;
using PJ::sdk::NoneTransform;
using PJ::sdk::OutlierRemovalTransform;
using PJ::sdk::Point2;
using PJ::sdk::registerAllTransforms;
using PJ::sdk::SamplesCounterTransform;
using PJ::sdk::ScaleTransform;
using PJ::sdk::SeriesAccessor;
using PJ::sdk::TimeSincePreviousTransform;

constexpr size_t kPreviewMaxPoints = 2000;

// Process-global "filter clipboard".
std::string& transformClipboard() {
  static std::string clipboard;
  return clipboard;
}

// Apply a FilterTransform over a SeriesAccessor, returning the output points.
std::string evaluate(FilterTransform& transform, const SeriesAccessor& input, std::vector<Point2>& out) {
  std::vector<Point2> in;
  in.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    in.push_back({input.timestamps[i], input.values[i]});
  }
  out = transform.applyBatch(in);
  return "";
}

// Read a numeric series's values as double, dispatching on its primitive type
// (the SDK only exposes typed accessors). Returns false for non-numeric
// (string/bool/unspecified) columns. PJ3 transforms operate on doubles, so we
// widen every integer/float column up front.
bool readValuesAsDouble(const PJ::sdk::MaterializedSeriesView& series, std::vector<double>& out) {
  const size_t n = series.timestamps().size();
  out.assign(n, 0.0);
  auto copy = [&](const auto* ptr) -> bool {
    if (ptr == nullptr) {
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      out[i] = static_cast<double>(ptr[i]);
    }
    return true;
  };
  using PT = PJ::PrimitiveType;
  switch (series.type()) {
    case PT::kFloat64:
      return copy(series.valuesAsFloat64());
    case PT::kFloat32:
      return copy(series.valuesAsFloat32());
    case PT::kInt8:
      return copy(series.valuesAsInt8());
    case PT::kInt16:
      return copy(series.valuesAsInt16());
    case PT::kInt32:
      return copy(series.valuesAsInt32());
    case PT::kInt64:
      return copy(series.valuesAsInt64());
    case PT::kUint8:
      return copy(series.valuesAsUint8());
    case PT::kUint16:
      return copy(series.valuesAsUint16());
    case PT::kUint32:
      return copy(series.valuesAsUint32());
    case PT::kUint64:
      return copy(series.valuesAsUint64());
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// FilterEditorDialog
// ---------------------------------------------------------------------------

class FilterEditorDialog : public PJ::DialogPluginTyped {
 public:
  using ApplyFn = std::function<std::string()>;

  std::string manifest() const override {
    return kFilterEditorManifest;
  }
  std::string ui_content() const override {
    return kFilterEditorDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setListItems("series_list", available_series_);
    if (!plot_colors_.empty()) {
      std::vector<std::string> colors;
      colors.reserve(available_series_.size());
      for (const auto& name : available_series_) {
        auto it = plot_colors_.find(name);
        colors.push_back((it != plot_colors_.end()) ? it->second : std::string{});
      }
      wd.setListItemColors("series_list", colors);
    }
    if (!selected_series_list_.empty()) {
      wd.setSelectedItems("series_list", selected_series_list_);
    }

    // Transform list — built from the factory registry (no hardcoded enum).
    {
      const auto ids = FilterTransformFactory::instance().registeredIds();
      std::vector<std::string> labels;
      labels.reserve(ids.size());
      std::string selected_label = transform_ ? std::string(transform_->label()) : "-- No Transform --";
      for (const auto& tid : ids) {
        auto t = FilterTransformFactory::instance().create(tid);
        if (t) {
          labels.push_back(t->label());
        }
      }
      wd.setListItems("transform_list", labels);
      wd.setSelectedItems("transform_list", {selected_label});
    }

    // Show only the relevant parameter panel.
    const std::string tid = transform_ ? std::string(transform_->id()) : "none";
    wd.setVisible("panel_scale", tid == "scale");
    wd.setVisible("panel_dt", tid == "derivative" || tid == "integral");
    wd.setVisible("panel_window", tid == "moving_average" || tid == "moving_rms" || tid == "moving_variance");
    wd.setVisible("center_check", tid == "moving_average");
    wd.setVisible("stddev_check", tid == "moving_variance");
    wd.setVisible("stddevHint", tid == "moving_variance");
    wd.setVisible("windowVarianceRow", tid == "moving_variance");
    wd.setVisible("panel_outlier", tid == "outlier_removal");
    wd.setVisible("panel_samples", tid == "samples_counter");
    wd.setVisible("panel_binary", tid == "binary_filter");
    // bin_b only meaningful for Range mode
    {
      auto* bf = dynamic_cast<BinaryFilterTransform*>(transform_.get());
      wd.setVisible("bin_b", bf != nullptr && bf->op == BinaryOp::kRange);
    }

    // Parameter widget values — read from the owning transform class.
    if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
      wd.setText("scale_value", trimDouble(t->value_scale));
      wd.setText("scale_voffset", trimDouble(t->value_offset));
      wd.setText("scale_toffset", trimDouble(t->time_offset));
    }
    if (auto* t = dynamic_cast<DerivativeTransform*>(transform_.get())) {
      wd.setChecked("dt_actual", !t->use_custom_dt);
      wd.setChecked("dt_custom", t->use_custom_dt);
      wd.setText("dt_value", trimDouble(t->custom_dt));
    }
    if (auto* t = dynamic_cast<IntegralTransform*>(transform_.get())) {
      wd.setChecked("dt_actual", !t->use_custom_dt);
      wd.setChecked("dt_custom", t->use_custom_dt);
      wd.setText("dt_value", trimDouble(t->custom_dt));
    }
    if (auto* t = dynamic_cast<MovingAverageTransform*>(transform_.get())) {
      wd.setValue("window_spin", t->window);
      wd.setChecked("center_check", t->compensate_time_offset);
    }
    if (auto* t = dynamic_cast<MovingRMSTransform*>(transform_.get())) {
      wd.setValue("window_spin", t->window);
    }
    if (auto* t = dynamic_cast<MovingVarianceTransform*>(transform_.get())) {
      wd.setValue("window_spin", t->window);
      wd.setChecked("stddev_check", t->std_dev);
    }
    if (auto* t = dynamic_cast<OutlierRemovalTransform*>(transform_.get())) {
      wd.setValue("outlier_spin", t->outlier_factor);
    }
    if (auto* t = dynamic_cast<SamplesCounterTransform*>(transform_.get())) {
      wd.setValue("samples_spin", t->samples_ms);
    }
    if (auto* t = dynamic_cast<BinaryFilterTransform*>(transform_.get())) {
      wd.setChecked("bin_eq", t->op == BinaryOp::kEqual);
      wd.setChecked("bin_lt", t->op == BinaryOp::kLess);
      wd.setChecked("bin_le", t->op == BinaryOp::kLessEq);
      wd.setChecked("bin_gt", t->op == BinaryOp::kGreater);
      wd.setChecked("bin_ge", t->op == BinaryOp::kGreaterEq);
      wd.setChecked("bin_range", t->op == BinaryOp::kRange);
      wd.setText("bin_a", trimDouble(t->a));
      wd.setText("bin_b", trimDouble(t->b));
    }

    // Alias field: auto-fill with "source[TransformName]" when the user has
    // not manually edited it (same behaviour as PJ3 lineEditAlias).
    const bool has_transform = (transform_ && std::string(transform_->id()) != "none");
    wd.setEnabled("alias_edit", has_transform && !selected_series_list_.empty());
    if (!alias_edited_by_user_ || alias_.empty()) {
      alias_ = has_transform ? (primarySeries() + "[" + transform_->bracketLabel() + "]") : std::string{};
    }
    wd.setText("alias_edit", alias_);

    wd.setChecked("autozoom_check", autozoom_);
    wd.setText("status_label", status_msg_);
    wd.setEnabled("save_btn", isValid());
    wd.setEnabled("generate_btn", isValid());
    wd.setEnabled("paste_btn", !transformClipboard().empty());

    wd.setChartZoomEnabled("chart_preview", !autozoom_);
    wd.setChartSeries("chart_preview", computePreview());

    if (!pending_close_.empty()) {
      const std::string reason = pending_close_;
      pending_close_.clear();
      wd.requestClose(reason);
    }
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "series_list" && !items.empty()) {
      selected_series_list_ = items;
      alias_edited_by_user_ = false;  // reset alias when source changes
      preview_dirty_ = true;
      return true;
    }
    if (name == "transform_list" && !items.empty()) {
      const auto ids = FilterTransformFactory::instance().registeredIds();
      for (const auto& tid : ids) {
        auto t = FilterTransformFactory::instance().create(tid);
        if (t && std::string(t->label()) == items.front()) {
          transform_ = std::move(t);
          break;
        }
      }
      alias_edited_by_user_ = false;
      preview_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    const std::string value(text);
    if (name == "alias_edit") {
      alias_ = value;
      alias_edited_by_user_ = true;
      return true;
    }
    if (name == "scale_value") {
      if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
        t->value_scale = toDouble(value, 1.0);
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "scale_voffset") {
      if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
        t->value_offset = toDouble(value, 0.0);
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "scale_toffset") {
      if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
        t->time_offset = toDouble(value, 0.0);
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "dt_value") {
      if (auto* td = dynamic_cast<DerivativeTransform*>(transform_.get())) {
        td->custom_dt = toDouble(value, 0.0);
      }
      if (auto* ti = dynamic_cast<IntegralTransform*>(transform_.get())) {
        ti->custom_dt = toDouble(value, 0.0);
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "bin_a") {
      if (auto* t = dynamic_cast<BinaryFilterTransform*>(transform_.get())) {
        t->a = toDouble(value, 0.0);
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "bin_b") {
      if (auto* t = dynamic_cast<BinaryFilterTransform*>(transform_.get())) {
        t->b = toDouble(value, 0.0);
      }
      preview_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onValueChanged(std::string_view name, int value) override {
    if (name == "window_spin") {
      if (auto* t = dynamic_cast<MovingAverageTransform*>(transform_.get())) {
        t->window = value;
      }
      if (auto* t = dynamic_cast<MovingRMSTransform*>(transform_.get())) {
        t->window = value;
      }
      if (auto* t = dynamic_cast<MovingVarianceTransform*>(transform_.get())) {
        t->window = value;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "samples_spin") {
      if (auto* t = dynamic_cast<SamplesCounterTransform*>(transform_.get())) {
        t->samples_ms = value;
      }
      preview_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onValueChanged(std::string_view name, double value) override {
    if (name == "outlier_spin") {
      if (auto* t = dynamic_cast<OutlierRemovalTransform*>(transform_.get())) {
        t->outlier_factor = value;
      }
      preview_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "autozoom_check") {
      autozoom_ = checked;
      return true;
    }
    if (name == "dt_actual" && checked) {
      if (auto* td = dynamic_cast<DerivativeTransform*>(transform_.get())) {
        td->use_custom_dt = false;
      }
      if (auto* ti = dynamic_cast<IntegralTransform*>(transform_.get())) {
        ti->use_custom_dt = false;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "dt_custom" && checked) {
      if (auto* td = dynamic_cast<DerivativeTransform*>(transform_.get())) {
        td->use_custom_dt = true;
      }
      if (auto* ti = dynamic_cast<IntegralTransform*>(transform_.get())) {
        ti->use_custom_dt = true;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "center_check") {
      if (auto* t = dynamic_cast<MovingAverageTransform*>(transform_.get())) {
        t->compensate_time_offset = checked;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "stddev_check") {
      if (auto* t = dynamic_cast<MovingVarianceTransform*>(transform_.get())) {
        t->std_dev = checked;
      }
      preview_dirty_ = true;
      return true;
    }
    if (checked) {
      static const std::unordered_map<std::string, BinaryOp> kOps = {
          {"bin_eq", BinaryOp::kEqual},   {"bin_lt", BinaryOp::kLess},      {"bin_le", BinaryOp::kLessEq},
          {"bin_gt", BinaryOp::kGreater}, {"bin_ge", BinaryOp::kGreaterEq}, {"bin_range", BinaryOp::kRange},
      };
      if (auto it = kOps.find(std::string(name)); it != kOps.end()) {
        if (auto* t = dynamic_cast<BinaryFilterTransform*>(transform_.get())) {
          t->op = it->second;
        }
        preview_dirty_ = true;
        return true;
      }
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "scale_deg2rad") {  // objectName kept; button now says "Rad to Deg"
      if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
        t->value_scale = 180.0 / 3.14159265358979;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "scale_rad2deg") {  // objectName kept; button now says "Deg to Rad"
      if (auto* t = dynamic_cast<ScaleTransform*>(transform_.get())) {
        t->value_scale = 3.14159265358979 / 180.0;
      }
      preview_dirty_ = true;
      return true;
    }
    if (name == "dt_guess") {
      // Guess dT: estimate the median time delta from the primary series,
      // filtering outliers (same intent as PJ3's buttonCompute).
      const std::string primary = primarySeries();
      auto it = series_data_.find(primary);
      if (it != series_data_.end() && it->second.size() >= 2) {
        const auto& ts = it->second.timestamps;
        std::vector<double> deltas;
        deltas.reserve(ts.size() - 1);
        for (size_t i = 1; i < ts.size(); ++i) {
          deltas.push_back(ts[i] - ts[i - 1]);
        }
        std::sort(deltas.begin(), deltas.end());
        const double median = deltas[deltas.size() / 2];
        if (median > 0.0) {
          if (auto* td = dynamic_cast<DerivativeTransform*>(transform_.get())) {
            td->custom_dt = median;
          }
          if (auto* ti = dynamic_cast<IntegralTransform*>(transform_.get())) {
            ti->custom_dt = median;
          }
          preview_dirty_ = true;
        }
      }
      return true;
    }
    if (name == "save_btn" && isValid()) {
      status_msg_ = save_fn_ ? save_fn_() : std::string("Save unavailable: toolbox not bound");
      pending_close_ = "saved";
      return true;
    }
    if (name == "copy_btn") {
      copyRecipe();
      status_msg_ = "Filter copied to clipboard";
      return true;
    }
    if (name == "paste_btn") {
      status_msg_ = pasteRecipe() ? "Filter pasted from clipboard" : "Clipboard is empty";
      return true;
    }
    if (name == "reset_btn") {
      transform_ = std::make_unique<NoneTransform>();
      preview_dirty_ = true;
      status_msg_ = reset_fn_ ? reset_fn_() : std::string("Reset");
      return true;
    }
    if (name == "generate_btn" && isValid()) {
      // Generate time series: apply the filter and write the result to the
      // datastore so it appears as a new persistent series in the curve tree.
      // Unlike Save (volatile), this creates a real Topic that survives the
      // toolbox panel closing.
      status_msg_ = generate_fn_ ? generate_fn_() : std::string("Generate unavailable: toolbox not bound");
      return true;
    }
    if (name == "cancel_btn") {
      if (cancel_fn_) {
        cancel_fn_();
      }
      pending_close_ = "cancelled";
      return true;
    }
    return false;
  }

  // Serialize the current filter recipe (transform + params + code, WITHOUT the
  // source binding) into the process-global clipboard.
  [[nodiscard]] bool isValid() const {
    return !selected_series_list_.empty() && transform_ && std::string(transform_->id()) != "none";
  }

  void setSaveCallback(ApplyFn fn) {
    save_fn_ = std::move(fn);
  }
  void setCancelCallback(std::function<void()> fn) {
    cancel_fn_ = std::move(fn);
  }
  void setResetCallback(ApplyFn fn) {
    reset_fn_ = std::move(fn);
  }
  void setGenerateCallback(ApplyFn fn) {
    generate_fn_ = std::move(fn);
  }
  void setStatus(std::string msg) {
    status_msg_ = std::move(msg);
  }

  void resetToNoTransform() {
    transform_ = std::make_unique<NoneTransform>();
    alias_.clear();
    alias_edited_by_user_ = false;
    preview_dirty_ = true;
    status_msg_.clear();
  }

  void setAvailableSeries(std::vector<std::string> names, std::unordered_map<std::string, SeriesAccessor> data) {
    series_data_ = std::move(data);
    if (!plot_curves_.empty()) {
      available_series_.clear();
      for (const auto& name : names) {
        if (std::find(plot_curves_.begin(), plot_curves_.end(), name) != plot_curves_.end()) {
          available_series_.push_back(name);
        }
      }
    } else {
      available_series_ = std::move(names);
    }
    std::string primary;
    for (const auto& s : selected_series_list_) {
      if (series_data_.find(s) != series_data_.end()) {
        primary = s;
        break;
      }
    }
    if (primary.empty()) {
      for (const auto& name : available_series_) {
        if (series_data_.find(name) != series_data_.end()) {
          primary = name;
          break;
        }
      }
    }
    selected_series_list_ = primary.empty() ? std::vector<std::string>{} : std::vector<std::string>{primary};
    preview_dirty_ = true;
  }

  void copyRecipe() const {
    auto j = nlohmann::json::parse(saveConfig(), nullptr, false);
    if (j.is_discarded()) {
      return;
    }
    j.erase("sources");
    j.erase("source");
    transformClipboard() = j.dump();
  }

  // Load a copied recipe onto the currently-selected sources. The recipe has no
  // "sources" key, so the current binding is preserved.
  bool pasteRecipe() {
    if (transformClipboard().empty()) {
      return false;
    }
    const auto keep_sources = selected_series_list_;
    loadConfig(transformClipboard());
    selected_series_list_ = keep_sources;
    preview_dirty_ = true;
    return true;
  }

  std::string saveConfig() const override {
    nlohmann::json j;
    // "sources" is the canonical key (multi-selection); "source" is kept for
    // backward compatibility with single-selection configs.
    j["sources"] = selected_series_list_;
    if (selected_series_list_.size() == 1) {
      j["source"] = selected_series_list_.front();
    }
    j["transform"] = transform_ ? std::string(transform_->id()) : "none";
    if (transform_) {
      auto ps = nlohmann::json::parse(transform_->saveParams(), nullptr, false);
      if (!ps.is_discarded()) {
        j["transform_params"] = ps;
      }
    }
    j["alias"] = alias_;
    j["alias_edited"] = alias_edited_by_user_;
    j["autozoom"] = autozoom_;
    // params now persisted per-transform via transform_->saveParams()
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) {
      return false;
    }
    // __plot_curves is runtime context injected by the host at launch time with
    // the curves from the specific plot that was right-clicked. It is NOT part
    // of the persistent config and must never be written back by saveConfig().
    if (j.contains("__plot_curves") && j["__plot_curves"].is_array()) {
      plot_curves_.clear();
      for (const auto& v : j["__plot_curves"]) {
        if (v.is_string()) {
          plot_curves_.push_back(v.get<std::string>());
        }
      }
    }
    if (j.contains("__plot_colors") && j["__plot_colors"].is_object()) {
      plot_colors_.clear();
      for (const auto& [name, color] : j["__plot_colors"].items()) {
        if (color.is_string()) {
          plot_colors_[name] = color.get<std::string>();
        }
      }
    }
    // Read multi-selection ("sources") or fall back to single ("source").
    selected_series_list_.clear();
    if (j.contains("sources") && j["sources"].is_array()) {
      for (const auto& v : j["sources"]) {
        if (v.is_string()) {
          selected_series_list_.push_back(v.get<std::string>());
        }
      }
    } else {
      const auto s = j.value("source", std::string{});
      if (!s.empty()) {
        selected_series_list_.push_back(s);
      }
    }
    {
      const std::string tid = j.value("transform", std::string{"none"});
      transform_ = FilterTransformFactory::instance().create(tid);
      if (!transform_) {
        transform_ = std::make_unique<NoneTransform>();
      }
      if (j.contains("transform_params")) {
        transform_->loadParams(j["transform_params"].dump());
      }
    }
    alias_ = j.value("alias", std::string{});
    alias_edited_by_user_ = j.value("alias_edited", false);
    autozoom_ = j.value("autozoom", true);
    // params restored per-transform via transform_->loadParams()
    preview_dirty_ = true;
    status_msg_.clear();
    return true;
  }

  // Returns all selected series (used by the toolbox to drive multi-apply).
  [[nodiscard]] const std::vector<std::string>& sourceSeriesList() const {
    return selected_series_list_;
  }
  // Primary series for the preview (first selected).
  [[nodiscard]] std::string primarySeries() const {
    return selected_series_list_.empty() ? std::string{} : selected_series_list_.front();
  }
  /// Access the current transform instance (for the toolbox streaming path).
  [[nodiscard]] FilterTransform* currentTransform() const {
    return transform_.get();
  }
  // Auto-derived output name, PJ3-style: "<source>[<Transform>]".
  [[nodiscard]] std::string outputName() const {
    if (!transform_ || std::string(transform_->id()) == "none") {
      return primarySeries();
    }
    return primarySeries() + "[" + transform_->bracketLabel() + "]";
  }

  static double toDouble(const std::string& s, double fallback) {
    try {
      size_t pos = 0;
      double v = std::stod(s, &pos);
      return v;
    } catch (...) {
      return fallback;
    }
  }

  static std::string trimDouble(double v) {
    std::string s = std::to_string(v);
    // strip trailing zeros for a tidier line edit
    if (s.find('.') != std::string::npos) {
      while (!s.empty() && s.back() == '0') {
        s.pop_back();
      }
      if (!s.empty() && s.back() == '.') {
        s.pop_back();
      }
    }
    return s;
  }

  std::vector<PJ::ChartSeries> computePreview() {
    if (!preview_dirty_) {
      return cached_preview_;
    }
    preview_dirty_ = false;
    cached_preview_.clear();

    const std::string primary = primarySeries();
    auto it = series_data_.find(primary);
    if (it == series_data_.end() || it->second.empty()) {
      return cached_preview_;
    }
    const SeriesAccessor& selected_input = it->second;

    // Use the first timestamp of the selected curve as the common time origin
    // so all series in the preview share the same x=0 reference (PJ3 behaviour).
    const double t0 = selected_input.timestamps.front();

    auto to_chart =
        [t0](const std::vector<Point2>& pts, const std::string& label, const std::string& color) -> PJ::ChartSeries {
      PJ::ChartSeries cs;
      cs.label = label;
      cs.color = color;
      const size_t step = (pts.size() > kPreviewMaxPoints) ? (pts.size() / kPreviewMaxPoints) : 1;
      for (size_t i = 0; i < pts.size(); i += step) {
        cs.points.push_back({(pts[i].x - t0) / 1e9, pts[i].y});
      }
      return cs;
    };

    // Blend a "#rrggbb" color towards white (factor > 0) or black (factor < 0).
    // factor = 0.65 → lighten 65% towards white (dim for context curves).
    // factor = -0.5 → darken 50% towards black (for the transform output).
    auto blendColor = [](const std::string& hex, float factor) -> std::string {
      if (hex.size() < 7 || hex[0] != '#') {
        return hex;
      }
      auto parse = [&](size_t pos) { return static_cast<int>(std::stoul(hex.substr(pos, 2), nullptr, 16)); };
      auto blend = [factor](int ch) -> int {
        if (factor >= 0.0f) {
          return static_cast<int>(static_cast<float>(ch) + factor * (255.0f - static_cast<float>(ch)));
        }
        return static_cast<int>(static_cast<float>(ch) * (1.0f + factor));
      };
      char buf[8];
      std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", blend(parse(1)), blend(parse(3)), blend(parse(5)));
      return std::string(buf);
    };

    auto curveColor = [this](const std::string& name, const std::string& fallback) -> std::string {
      auto color_it = plot_colors_.find(name);
      return (color_it != plot_colors_.end() && !color_it->second.empty()) ? color_it->second : fallback;
    };

    // Context curves: real color dimmed towards white so they recede visually.
    // Curves in selected_series_list_ but not primary are also shown dimmed
    // (the user sees which curves will be affected).
    const std::vector<std::string>& context_curves = plot_curves_.empty() ? available_series_ : plot_curves_;
    for (const auto& name : context_curves) {
      if (name == primary) {
        continue;  // drawn separately below
      }
      auto ctx_it = series_data_.find(name);
      if (ctx_it == series_data_.end() || ctx_it->second.empty()) {
        continue;
      }
      const SeriesAccessor& ctx = ctx_it->second;
      std::vector<Point2> ctx_pts;
      ctx_pts.reserve(ctx.size());
      for (size_t i = 0; i < ctx.size(); ++i) {
        ctx_pts.push_back({ctx.timestamps[i], ctx.values[i]});
      }
      const std::string dim = blendColor(curveColor(name, "#aaaaaa"), 0.65f);
      cached_preview_.push_back(to_chart(ctx_pts, name, dim));
    }

    // Primary selected curve — full real color, drawn on top of context.
    std::vector<Point2> in_pts;
    in_pts.reserve(selected_input.size());
    for (size_t i = 0; i < selected_input.size(); ++i) {
      in_pts.push_back({selected_input.timestamps[i], selected_input.values[i]});
    }
    const std::string source_color = curveColor(primary, "#333333");
    cached_preview_.push_back(to_chart(in_pts, primary, source_color));

    // Transform output — same color as source but darker, so it reads as
    // "this is the same curve, transformed" rather than a foreign overlay.
    std::vector<Point2> output;
    if (!transform_ || std::string(transform_->id()) == "none") {
      output = in_pts;
    } else {
      std::string err = evaluate(*transform_, selected_input, output);
      if (!err.empty()) {
        status_msg_ = "Preview error: " + err;
        return cached_preview_;
      }
    }
    status_msg_.clear();
    // Darken the source color for the transform output so it contrasts with
    // the original without introducing an unrelated hue.
    const std::string transform_color = blendColor(source_color, -0.5f);  // negative = darken
    cached_preview_.push_back(to_chart(output, outputName(), transform_color));

    return cached_preview_;
  }

  std::vector<std::string> available_series_;
  std::unordered_map<std::string, SeriesAccessor> series_data_;
  std::vector<std::string> selected_series_list_;  // multi-selection
  // Curves from the plot that launched this editor (right-click context).
  // Empty = show all datastore series (launched from toolbox panel).
  std::vector<std::string> plot_curves_;
  // Colors of those curves: human_name -> "#rrggbb". Used in the preview.
  std::unordered_map<std::string, std::string> plot_colors_;
  // Current transform instance (owns its parameters). Never null.
  std::unique_ptr<FilterTransform> transform_ = std::make_unique<NoneTransform>();
  // Alias: custom legend name for the transformed curve (PJ3 lineEditAlias).
  // Auto-filled with "source[TransformName]"; user can override it.
  std::string alias_;
  bool alias_edited_by_user_ = false;
  bool autozoom_ = true;
  std::string status_msg_;

  bool preview_dirty_ = true;
  std::vector<PJ::ChartSeries> cached_preview_;
  ApplyFn save_fn_;
  std::function<void()> cancel_fn_;
  ApplyFn reset_fn_;
  ApplyFn generate_fn_;
  std::string pending_close_;
};

// ---------------------------------------------------------------------------
// FilterEditorToolbox
// ---------------------------------------------------------------------------

class FilterEditorToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Ensure all transforms are registered before the dialog is shown.
    registerAllTransforms();
    if (toolboxHostBound()) {
      std::vector<std::string> names;
      std::unordered_map<std::string, SeriesAccessor> data;
      readCatalog(names, data);
      dialog_.setAvailableSeries(std::move(names), std::move(data));
    }
    // Snapshot config at open so Cancel can revert the edits.
    config_at_open_ = dialog_.saveConfig();
    dialog_.setSaveCallback([this]() { return applyAndReport(); });
    dialog_.setCancelCallback([this]() { dialog_.loadConfig(config_at_open_); });
    dialog_.setResetCallback([this]() { return resetAndReport(); });
    dialog_.setGenerateCallback([this]() { return generateAndReport(); });
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

  void onDataChanged() override {
    // In the volatile model the transform lives in the DatastoreCurveAdapter
    // read path (host side). The host re-applies it on every notifyDataChanged
    // call, so the plugin side has nothing to do here — the streaming update
    // path is handled by the adapter's onTopicCommitted invalidation.
  }

 private:
  void readCatalog(std::vector<std::string>& names, std::unordered_map<std::string, SeriesAccessor>& data) {
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return;
    }
    auto all_fields = catalog->fields();
    for (const auto& topic : catalog->topics()) {
      std::string topic_name(PJ::sdk::toStringView(topic.name));
      for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
        const auto& f = all_fields[fi];
        std::string name = topic_name + "/" + std::string(PJ::sdk::toStringView(f.name));
        names.push_back(name);
        auto series = host.readSeries(f.handle);
        if (series) {
          std::vector<double> values;
          if (readValuesAsDouble(*series, values)) {
            auto ts = series->timestamps();
            SeriesAccessor acc;
            acc.timestamps.resize(ts.size());
            for (size_t i = 0; i < ts.size(); ++i) {
              acc.timestamps[i] = static_cast<double>(ts[i]);
            }
            acc.values = std::move(values);
            data[name] = std::move(acc);
          }
        }
      }
    }
    std::sort(names.begin(), names.end());
  }

  PJ::Expected<SeriesAccessor> readSource(const std::string& full_name) {
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return PJ::unexpected("failed to acquire catalog");
    }
    auto all_fields = catalog->fields();
    for (const auto& topic : catalog->topics()) {
      std::string topic_name(PJ::sdk::toStringView(topic.name));
      for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
        const auto& f = all_fields[fi];
        std::string qualified = topic_name + "/" + std::string(PJ::sdk::toStringView(f.name));
        if (qualified != full_name) {
          continue;
        }
        auto series = host.readSeries(f.handle);
        if (!series) {
          return PJ::unexpected("failed to read series: " + full_name);
        }
        std::vector<double> values;
        if (!readValuesAsDouble(*series, values)) {
          return PJ::unexpected("series is not numeric: " + full_name);
        }
        auto ts = series->timestamps();
        SeriesAccessor acc;
        acc.timestamps.resize(ts.size());
        for (size_t i = 0; i < ts.size(); ++i) {
          acc.timestamps[i] = static_cast<double>(ts[i]);
        }
        acc.values = std::move(values);
        return acc;
      }
    }
    return PJ::unexpected("series not found: " + full_name);
  }

  // Called from the dialog's "Save" button. The filter is applied as a
  // volatile transform in the DatastoreCurveAdapter read path (host side),
  // so no Topic is written to the datastore. notifyDataChanged() triggers
  // the host's on_data_changed callback which builds and installs the
  // TransformFn from the current saveConfig() JSON.
  std::string applyAndReport() {
    if (!runtimeHostBound()) {
      return "Toolbox not bound to a host";
    }
    created_ = true;
    runtimeHost().notifyDataChanged();
    return "Applied '" + dialog_.outputName() + "'";
  }

  // Called from the dialog's "Reset" button.
  std::string resetAndReport() {
    dialog_.resetToNoTransform();
    created_ = false;
    last_applied_config_.clear();
    // Notify the host with "none" transform so it clears the volatile transform
    // from the DatastoreCurveAdapter — the original curve is restored instantly.
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    return "Reset";
  }

  // Called from the dialog's "Generate time series" button.
  // Applies the filter to ALL selected sources and writes each result as a
  // new persistent series in the datastore — it appears in the curve tree.
  // Unlike Save (volatile), this series survives the panel closing.
  std::string generateAndReport() {
    if (!toolboxHostBound() || !runtimeHostBound()) {
      return "Toolbox not bound to a host";
    }
    std::vector<std::string> created_names;
    for (const auto& series_name : dialog_.sourceSeriesList()) {
      auto source = readSource(series_name);
      if (!source) {
        continue;
      }
      auto* transform = dialog_.currentTransform();
      if (!transform || std::string(transform->id()) == "none") {
        continue;
      }

      std::vector<Point> in, output;
      in.reserve(source->size());
      for (size_t i = 0; i < source->size(); ++i) {
        in.push_back({source->timestamps[i], source->values[i]});
      }
      output = transform->applyBatch(in);
      if (output.empty()) {
        continue;
      }

      const std::string out_name = series_name + "[" + transform->bracketLabel() + "]";
      auto host = toolboxHost();
      auto src = host.createDataSource("filter_editor_generated");
      if (!src) {
        continue;
      }
      auto topic = host.ensureTopic(*src, out_name);
      if (!topic) {
        continue;
      }
      for (const auto& pt : output) {
        const PJ::sdk::NamedFieldValue fields[] = {{.name = "value", .value = pt.y}};
        (void)host.appendRecord(*topic, static_cast<int64_t>(pt.x), PJ::Span<const PJ::sdk::NamedFieldValue>(fields));
      }
      created_names.push_back(out_name);
    }
    runtimeHost().notifyDataChanged();
    if (created_names.empty()) {
      return "No series generated";
    }
    std::string msg = "Generated: ";
    for (size_t i = 0; i < created_names.size(); ++i) {
      if (i > 0) {
        msg += ", ";
      }
      msg += "'" + created_names[i] + "'";
    }
    return msg;
  }

  PJ::Status applyTransformToStore(bool reset_if_changed) {
    auto host = toolboxHost();

    auto source = readSource(dialog_.primarySeries());
    if (!source) {
      return PJ::unexpected(source.error());
    }

    // Detect config changes (transform, params, source, output name) and reset
    // the append cursor + output handles so a re-create replays from scratch.
    std::string config = dialog_.saveConfig();
    if (reset_if_changed && config != last_applied_config_) {
      resetIncrementalState();
    }
    last_applied_config_ = config;

    const size_t source_count = source->size();
    if (source_count < consumed_input_count_) {
      resetIncrementalState();  // reload / replacement shrank the series
    }
    // "If nothing changes, don't recompute" — no new samples to process.
    if (created_ && source_count == consumed_input_count_) {
      return PJ::okStatus();
    }

    // Streaming: use the transform's isStreamSafe() to decide whether to
    // recompute only the new tail (safe) or the full series (unsafe).
    // We always do full-batch via applyBatch for correctness; the incremental
    // path (consumed_input_count_) still avoids re-appending old points.
    const bool monotonic = dialog_.currentTransform() && dialog_.currentTransform()->isStreamSafe();
    (void)monotonic;  // reserved for T3 streaming optimisation

    SeriesAccessor full_src = *source;

    std::vector<Point2> output;
    if (dialog_.currentTransform()) {
      std::string err = evaluate(*dialog_.currentTransform(), full_src, output);
      if (!err.empty()) {
        return PJ::unexpected(err);
      }
    }

    if (!source_handle_) {
      auto src = host.createDataSource("transform_editor");
      if (!src) {
        return PJ::unexpected("failed to create output data source");
      }
      source_handle_ = *src;
    }
    if (!topic_handle_) {
      auto topic = host.ensureTopic(*source_handle_, dialog_.outputName());
      if (!topic) {
        return PJ::unexpected("failed to create output topic");
      }
      topic_handle_ = *topic;
    }

    auto appendPoint = [&](const Point2& pt) -> PJ::Status {
      const PJ::sdk::NamedFieldValue fields[] = {
          {.name = "value", .value = pt.y},
      };
      auto st = host.appendRecord(
          *topic_handle_, static_cast<int64_t>(pt.x), PJ::Span<const PJ::sdk::NamedFieldValue>(fields));
      if (!st) {
        return PJ::unexpected("failed to append record: " + std::string(st.error()));
      }
      return PJ::okStatus();
    };

    if (monotonic) {
      // The suffix overlaps the already-emitted region (look-back context);
      // those points carry timestamps <= last_appended_ts_ and are skipped.
      for (const auto& pt : output) {
        const auto ts = static_cast<int64_t>(pt.x);
        if (ts <= last_appended_ts_) {
          continue;
        }
        if (auto st = appendPoint(pt); !st) {
          return st;
        }
        last_appended_ts_ = ts;
      }
    } else {
      // Full recompute: suffix == full input, so output index == global index.
      for (size_t i = emitted_output_count_; i < output.size(); ++i) {
        if (auto st = appendPoint(output[i]); !st) {
          return st;
        }
      }
      emitted_output_count_ = output.size();
    }
    consumed_input_count_ = source_count;
    runtimeHost().notifyDataChanged();
    return PJ::okStatus();
  }

  void resetIncrementalState() {
    source_handle_ = std::nullopt;
    topic_handle_ = std::nullopt;
    consumed_input_count_ = 0;
    last_appended_ts_ = std::numeric_limits<int64_t>::min();
    emitted_output_count_ = 0;
  }

  FilterEditorDialog dialog_;
  bool created_ = false;
  std::string config_at_open_;
  std::string last_applied_config_;
  std::optional<PJ::sdk::DataSourceHandle> source_handle_;
  std::optional<PJ::sdk::TopicHandle> topic_handle_;
  size_t consumed_input_count_ = 0;
  int64_t last_appended_ts_ = std::numeric_limits<int64_t>::min();
  size_t emitted_output_count_ = 0;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(FilterEditorToolbox, kFilterEditorManifest)
PJ_DIALOG_PLUGIN(FilterEditorDialog)
