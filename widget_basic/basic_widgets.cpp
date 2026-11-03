// The Qt header must precede PJ_WIDGET_PLUGIN: the macro reads QT_VERSION_* to fill build_info.
#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "basic_widgets_manifest.hpp"
#include "canvas.hpp"
#include "gauge_canvas.hpp"
#include "numeric_indicator_canvas.hpp"
#include "traffic_light_canvas.hpp"
#include "ui_gauge.h"
#include "ui_numeric_indicator.h"
#include "ui_traffic_light.h"
#include "ui_value_table.h"
#include "value_table_canvas.hpp"

namespace {

using basic_widgets::BasicCanvas;

/// Validated settings of one widget type, bound to the canvas class of that type.
using ApplySettings = std::function<void(BasicCanvas& canvas)>;

/// Validates @p settings for the canvas the form @p Ui promotes as "canvas".
template <typename Ui>
PJ::Expected<ApplySettings> parseFor(const nlohmann::json& settings) {
  using Canvas = std::remove_pointer_t<decltype(Ui::canvas)>;
  auto parsed = Canvas::parseSettings(settings);
  if (!parsed) {
    return PJ::unexpected(std::move(parsed).error());
  }
  return ApplySettings(
      [valid = std::move(parsed).value()](BasicCanvas& canvas) { static_cast<Canvas&>(canvas).applySettings(valid); });
}

/// Builds the form of @p Ui under @p root and returns its canvas.
template <typename Ui>
BasicCanvas* buildForm(QWidget* root) {
  Ui ui;
  ui.setupUi(root);
  return ui.canvas;
}

/// What a widget type needs: its inputs, and its settings and form functions, both instantiated from
/// the same form so the settings always apply to the canvas class they were validated for.
struct TypeEntry {
  std::string_view type;
  bool multi_series;
  PJ::Expected<ApplySettings> (*parse)(const nlohmann::json& settings);
  BasicCanvas* (*build)(QWidget* root);
};

template <typename Ui>
constexpr TypeEntry makeEntry(std::string_view type, bool multi_series) {
  return {type, multi_series, &parseFor<Ui>, &buildForm<Ui>};
}

constexpr TypeEntry kTypes[] = {
    makeEntry<Ui::GaugeForm>("gauge", false),
    makeEntry<Ui::NumericIndicatorForm>("numeric_indicator", false),
    makeEntry<Ui::TrafficLightForm>("traffic_light", false),
    makeEntry<Ui::ValueTableForm>("value_table", true),
};

const TypeEntry& entryFor(std::string_view type) {
  for (const auto& entry : kTypes) {
    if (entry.type == type) {
      return entry;
    }
  }
  throw std::invalid_argument("unknown widget type");
}

/// One class serves the four widget types of the library; `type` picks the form, the inputs and the
/// canvas. The canvas owns the drawing; this class validates the settings and moves data to it.
class BasicWidgets : public PJ::WidgetPluginBase {
 public:
  explicit BasicWidgets(std::string_view type)
      : entry_(entryFor(type)), apply_settings_(entry_.parse(nlohmann::json::object()).value()) {}

  ~BasicWidgets() override {
    // The host detaches the widget before destroying the instance; it may also have been deleted
    // already (QPointer is then null).
    delete root_.data();
  }

  std::vector<PJ::WidgetInput> inputs() const override {
    if (entry_.multi_series) {
      return {PJ::WidgetInput("rows", "Rows", "", PJ::WidgetInputKind::kNumber, true)};
    }
    return {PJ::WidgetInput("value", "Value")};
  }

  void* createWidget(void* parent) override {
    auto* root = new QWidget(static_cast<QWidget*>(parent));
    root_ = root;
    canvas_ = entry_.build(root);
    // The defaults or what loadConfig already validated: nothing here can reject them.
    apply_settings_(*canvas_);
    return root;
  }

  void onUpdate(PJ::Span<const PJ_widget_value_t> values, double /*time*/) override {
    if (canvas_) {
      canvas_->setSamples(values);
    }
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    const nlohmann::json parsed =
        config_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(config_json, nullptr, false);
    if (!parsed.is_object()) {
      return PJ::unexpected("configuration is not a JSON object");
    }
    // Validated whether the widget exists or not: the host restores the saved settings before it
    // creates the widget, and only a rejection here lets it report them and fall back to the defaults.
    auto settings = entry_.parse(parsed);
    if (!settings) {
      return PJ::unexpected(std::move(settings).error());
    }
    apply_settings_ = std::move(settings).value();
    if (canvas_) {
      apply_settings_(*canvas_);
    }
    return PJ::okStatus();
  }

 private:
  const TypeEntry& entry_;
  ApplySettings apply_settings_;
  QPointer<QWidget> root_;
  QPointer<BasicCanvas> canvas_;
};

}  // namespace

PJ_WIDGET_PLUGIN(BasicWidgets, kBasicWidgetsManifest)
