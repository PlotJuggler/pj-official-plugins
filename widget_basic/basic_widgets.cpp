// The Qt header must precede PJ_WIDGET_PLUGIN: the macro reads QT_VERSION_* to fill build_info.
#include <QPointer>
#include <QWidget>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "basic_widgets_manifest.hpp"
#include "canvas.hpp"
#include "ui_gauge.h"
#include "ui_numeric_indicator.h"
#include "ui_traffic_light.h"
#include "ui_value_table.h"

namespace {

using basic_widgets::BasicCanvas;

/// Builds the form of @p Ui under @p root and returns the promoted canvas it declares as "canvas".
template <typename Ui>
BasicCanvas* buildForm(QWidget* root) {
  Ui ui;
  ui.setupUi(root);
  return ui.canvas;
}

/// What a widget type needs: its inputs and the function that builds its form.
struct TypeEntry {
  std::string_view type;
  bool multi_series;
  BasicCanvas* (*build)(QWidget* root);
};

constexpr TypeEntry kTypes[] = {
    {"gauge", false, &buildForm<Ui::GaugeForm>},
    {"numeric_indicator", false, &buildForm<Ui::NumericIndicatorForm>},
    {"traffic_light", false, &buildForm<Ui::TrafficLightForm>},
    {"value_table", true, &buildForm<Ui::ValueTableForm>},
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
/// canvas. The canvas owns the drawing and the settings; this class only moves data to it.
class BasicWidgets : public PJ::WidgetPluginBase {
 public:
  explicit BasicWidgets(std::string_view type) : entry_(entryFor(type)) {}

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
    if (auto status = canvas_->applySettings(settings_); !status) {
      delete root;
      throw std::runtime_error(status.error());
    }
    return root;
  }

  void onUpdate(PJ::Span<const PJ_widget_value_t> values, double /*time*/) override {
    if (canvas_) {
      canvas_->setSamples(values);
    }
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    nlohmann::json parsed =
        config_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(config_json, nullptr, false);
    if (!parsed.is_object()) {
      return PJ::unexpected("configuration is not a JSON object");
    }
    if (canvas_) {
      if (auto status = canvas_->applySettings(parsed); !status) {
        return status;
      }
    }
    settings_ = std::move(parsed);
    return PJ::okStatus();
  }

 private:
  const TypeEntry& entry_;
  nlohmann::json settings_ = nlohmann::json::object();
  QPointer<QWidget> root_;
  QPointer<BasicCanvas> canvas_;
};

}  // namespace

PJ_WIDGET_PLUGIN(BasicWidgets, kBasicWidgetsManifest)
