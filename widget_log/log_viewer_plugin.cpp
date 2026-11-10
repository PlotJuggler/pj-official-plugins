// The Qt header must precede PJ_WIDGET_PLUGIN: the macro reads QT_VERSION_* to fill build_info.
#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log_viewer_manifest.hpp"
#include "log_viewer_widget.hpp"

namespace {

using log_viewer::FutureRows;
using log_viewer::ViewState;

// saveConfig keys. They share the load_config object with the manifest settings (`future_rows`), so none
// of them may reuse a setting key.
constexpr const char* kLevelsKey = "levels";
constexpr const char* kNodeKey = "node_filter";
constexpr const char* kTextKey = "text_filter";
constexpr const char* kHiddenTopicsKey = "hidden_topics";
constexpr const char* kSplitterKey = "splitter_sizes";
constexpr const char* kColumnsKey = "column_widths";
constexpr const char* kFutureRowsSetting = "future_rows";

constexpr std::array<const char*, 5> kLevelNames = {"debug", "info", "warn", "error", "fatal"};

nlohmann::json toJson(const ViewState& state) {
  nlohmann::json levels = nlohmann::json::array();
  for (size_t i = 0; i < kLevelNames.size(); ++i) {
    if (state.levels[i]) {
      levels.push_back(kLevelNames[i]);
    }
  }
  return nlohmann::json{
      {kLevelsKey, levels},           {kNodeKey, state.node},
      {kTextKey, state.text},         {kHiddenTopicsKey, state.hidden_topics},
      {kSplitterKey, state.splitter}, {kColumnsKey, state.columns},
  };
}

/// Lay the state keys present in @p config over @p state. A key of the wrong type is an error.
PJ::Status readState(const nlohmann::json& config, ViewState& state) {
  ViewState next = state;
  const auto string_list = [&](const char* key, std::vector<std::string>& out) -> PJ::Status {
    const auto found = config.find(key);
    if (found == config.end() || found->is_null()) {
      return PJ::okStatus();
    }
    if (!found->is_array()) {
      return PJ::unexpected(std::string(key) + " must be an array of strings");
    }
    std::vector<std::string> values;
    for (const auto& item : *found) {
      if (!item.is_string()) {
        return PJ::unexpected(std::string(key) + " must be an array of strings");
      }
      values.push_back(item.get<std::string>());
    }
    out = std::move(values);
    return PJ::okStatus();
  };
  const auto int_list = [&](const char* key, std::vector<int>& out) -> PJ::Status {
    const auto found = config.find(key);
    if (found == config.end() || found->is_null()) {
      return PJ::okStatus();
    }
    if (!found->is_array()) {
      return PJ::unexpected(std::string(key) + " must be an array of integers");
    }
    std::vector<int> values;
    for (const auto& item : *found) {
      if (!item.is_number_integer()) {
        return PJ::unexpected(std::string(key) + " must be an array of integers");
      }
      values.push_back(item.get<int>());
    }
    out = std::move(values);
    return PJ::okStatus();
  };
  const auto text = [&](const char* key, std::string& out) -> PJ::Status {
    const auto found = config.find(key);
    if (found == config.end() || found->is_null()) {
      return PJ::okStatus();
    }
    if (!found->is_string()) {
      return PJ::unexpected(std::string(key) + " must be a string");
    }
    out = found->get<std::string>();
    return PJ::okStatus();
  };

  std::vector<std::string> levels;
  bool has_levels = config.contains(kLevelsKey) && !config.at(kLevelsKey).is_null();
  for (auto status :
       {string_list(kLevelsKey, levels), text(kNodeKey, next.node), text(kTextKey, next.text),
        string_list(kHiddenTopicsKey, next.hidden_topics), int_list(kSplitterKey, next.splitter),
        int_list(kColumnsKey, next.columns)}) {
    if (!status) {
      return status;
    }
  }
  if (has_levels) {
    for (size_t i = 0; i < kLevelNames.size(); ++i) {
      next.levels[i] = std::find(levels.begin(), levels.end(), kLevelNames[i]) != levels.end();
    }
  }
  state = std::move(next);
  return PJ::okStatus();
}

class LogViewerPlugin : public PJ::WidgetPluginBase {
 public:
  ~LogViewerPlugin() override {
    // The host detaches the widget before destroying the instance; it may also have been deleted
    // already (QPointer is then null).
    delete widget_.data();
  }

  std::vector<PJ::WidgetInput> inputs() const override {
    return {PJ::WidgetInput("logs", "Logs", "", PJ::WidgetInputKind::kObject, true)};
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = WidgetPluginBase::bind(services);
    if (!status) {
      return status;
    }
    // Optional: an older host has no playback service, and the list then just scrolls freely.
    playback_ = services.get<PJ::sdk::PlaybackHostService>();
    return status;
  }

  void* createWidget(void* parent) override {
    auto query = [this](
                     uint32_t input, uint32_t slot, double from, double to, uint32_t max_samples,
                     const log_viewer::HistorySink& sink) {
      return widgetHost().queryHistory(input, slot, from, to, max_samples, sink);
    };
    auto warn = [this](const std::string& message) {
      widgetHost().reportWidgetMessage(PJ::WidgetMessageLevel::kWarning, message);
    };
    auto* widget = new log_viewer::LogViewerWidget(query, playback_, warn, static_cast<QWidget*>(parent));
    widget_ = widget;
    widget->setFutureRows(future_rows_);
    widget->applyState(state_);
    return widget;
  }

  void onUpdate(PJ::Span<const PJ_widget_value_t> values, double time) override {
    if (widget_) {
      widget_->onUpdate(values, time);
    }
  }

  std::string saveConfig() const override {
    return toJson(widget_ ? widget_->state() : state_).dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const nlohmann::json config =
        config_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(config_json, nullptr, false);
    if (!config.is_object()) {
      return PJ::unexpected("configuration is not a JSON object");
    }
    FutureRows future_rows = future_rows_;
    if (const auto found = config.find(kFutureRowsSetting); found != config.end() && !found->is_null()) {
      if (!found->is_string() || (*found != "grey" && *found != "hide")) {
        return PJ::unexpected("future_rows must be \"grey\" or \"hide\"");
      }
      future_rows = *found == "hide" ? FutureRows::kHide : FutureRows::kGrey;
    }
    ViewState state = widget_ ? widget_->state() : state_;
    if (auto status = readState(config, state); !status) {
      return status;
    }
    future_rows_ = future_rows;
    state_ = std::move(state);
    if (widget_) {
      widget_->setFutureRows(future_rows_);
      widget_->applyState(state_);
    }
    return PJ::okStatus();
  }

 private:
  std::optional<PJ::sdk::PlaybackHostView> playback_;
  FutureRows future_rows_ = FutureRows::kGrey;
  ViewState state_;
  QPointer<log_viewer::LogViewerWidget> widget_;
};

}  // namespace

PJ_WIDGET_PLUGIN(LogViewerPlugin, kLogViewerManifest)
