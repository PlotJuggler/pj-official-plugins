// The Qt header must precede PJ_WIDGET_PLUGIN: the macro reads QT_VERSION_* to fill build_info.
#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "log_viewer_manifest.hpp"
#include "log_viewer_widget.hpp"

namespace {

using log_viewer::FutureRows;
using log_viewer::kLevelNames;
using log_viewer::kToggleLevels;
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

nlohmann::json toJson(const ViewState& state) {
  nlohmann::json levels = nlohmann::json::array();
  for (size_t i = 0; i < kToggleLevels.size(); ++i) {
    if (state.levels[i]) {
      levels.push_back(kLevelNames[i].key);
    }
  }
  return nlohmann::json{
      {kLevelsKey, levels},           {kNodeKey, state.node},
      {kTextKey, state.text},         {kHiddenTopicsKey, state.hidden_topics},
      {kSplitterKey, state.splitter}, {kColumnsKey, state.columns},
  };
}

/// Read the array of strings or integers at @p key into @p out (a std::vector<T>, or a std::optional of
/// one to learn whether the key is there). An absent or null key leaves @p out alone.
template <typename T, typename Out>
PJ::Status readList(const nlohmann::json& config, const char* key, Out& out) {
  constexpr bool kStrings = std::is_same_v<T, std::string>;
  const auto found = config.find(key);
  if (found == config.end() || found->is_null()) {
    return PJ::okStatus();
  }
  const auto wrong_type = [key] {
    return PJ::unexpected(
        std::string(key) + (kStrings ? " must be an array of strings" : " must be an array of integers"));
  };
  if (!found->is_array()) {
    return wrong_type();
  }
  std::vector<T> values;
  for (const auto& item : *found) {
    if (kStrings ? !item.is_string() : !item.is_number_integer()) {
      return wrong_type();
    }
    values.push_back(item.template get<T>());
  }
  out = std::move(values);
  return PJ::okStatus();
}

/// Lay the state keys present in @p config over @p state. A key of the wrong type is an error.
PJ::Status readState(const nlohmann::json& config, ViewState& state) {
  ViewState next = state;
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

  std::optional<std::vector<std::string>> levels;
  for (auto status :
       {readList<std::string>(config, kLevelsKey, levels), text(kNodeKey, next.node), text(kTextKey, next.text),
        readList<std::string>(config, kHiddenTopicsKey, next.hidden_topics),
        readList<int>(config, kSplitterKey, next.splitter), readList<int>(config, kColumnsKey, next.columns)}) {
    if (!status) {
      return status;
    }
  }
  if (levels) {
    for (size_t i = 0; i < kToggleLevels.size(); ++i) {
      next.levels[i] = std::find(levels->begin(), levels->end(), kLevelNames[i].key) != levels->end();
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
    auto warn = [this](const std::string& message) {
      widgetHost().reportWidgetMessage(PJ::WidgetMessageLevel::kWarning, message);
    };
    auto* widget = new log_viewer::LogViewerWidget(widgetHost(), playback_, warn, static_cast<QWidget*>(parent));
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
