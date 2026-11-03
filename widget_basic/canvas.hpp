#pragma once

#include <pj_plugins/widget_protocol.h>

#include <QColor>
#include <QFont>
#include <QString>
#include <QWidget>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

namespace basic_widgets {

/// Base of the four widgets' custom-painted surface. Each concrete canvas is promoted in its own .ui
/// file, so the form creates it and the plugin only talks to this interface.
///
/// Settings are split in two steps on every concrete canvas: a static `parseSettings(json)` that
/// validates the complete settings object without touching Qt, so it runs before the widget exists,
/// and an `applySettings(Settings)` that cannot fail. Keys missing from the object take their manifest
/// default (the member initializers of each `Settings`), so a setting the user cleared does not keep
/// its previous value.
///
/// Everything a canvas draws is read from the samples copied in setSamples(), never from the host
/// during a paint: the pointers inside a PJ_widget_value_t die when onUpdate returns.
class BasicCanvas : public QWidget {
 public:
  explicit BasicCanvas(QWidget* parent) : QWidget(parent) {}

  /// Copy what is needed out of @p values and schedule a repaint.
  virtual void setSamples(PJ::Span<const PJ_widget_value_t> values) = 0;
};

/// A canvas that shows one number, the first wire of input 0.
class NumberCanvas : public BasicCanvas {
 public:
  using BasicCanvas::BasicCanvas;

  void setSamples(PJ::Span<const PJ_widget_value_t> values) override;

 protected:
  std::optional<double> value_;
};

/// Status colours. These are the one thing not taken from the palette: green, amber and red must
/// mean the same in a light and a dark theme, and QPalette has no roles for them.
QColor goodColor();
QColor warnColor();
QColor badColor();

/// The value of the first wire of input @p input, when it holds a finite number.
std::optional<double> numberAt(PJ::Span<const PJ_widget_value_t> values, uint32_t input);

/// Largest font, not above @p base's size and not below 6 px, whose @p text fits @p box.
QFont fittedFont(const QFont& base, const QString& text, const QSizeF& box, double max_pixel_size);

/// Read one settings key into @p out. A key that is missing or null leaves @p out as it is (the caller
/// initializes it to the manifest default); a key present with the wrong JSON type is an error.
PJ::Status readNumber(const nlohmann::json& settings, const char* key, double& out);
/// As readNumber, rounded and clamped to [@p min, @p max].
PJ::Status readInteger(const nlohmann::json& settings, const char* key, int min, int max, int& out);
PJ::Status readText(const nlohmann::json& settings, const char* key, std::string& out);
/// The `decimals` setting every number-showing widget declares (0..12, as in the manifests).
PJ::Status readDecimals(const nlohmann::json& settings, int& out);
/// The first failed status of @p statuses, or ok. Meant for a list of reads, which are all evaluated.
PJ::Status firstError(std::initializer_list<PJ::Status> statuses);

/// A measurement with fixed decimals, or "--" when there is no sample.
QString formatNumber(std::optional<double> value, int decimals);

}  // namespace basic_widgets
