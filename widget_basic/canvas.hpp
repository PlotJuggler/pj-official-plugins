#pragma once

#include <pj_plugins/widget_protocol.h>

#include <QColor>
#include <QFont>
#include <QString>
#include <QWidget>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>

namespace basic_widgets {

/// Base of the four widgets' custom-painted surface. Each concrete canvas is promoted in its own .ui
/// file, so the form creates it and the plugin only talks to this interface.
///
/// Everything a canvas draws is read from the samples copied in setSamples(), never from the host
/// during a paint: the pointers inside a PJ_widget_value_t die when onUpdate returns.
class BasicCanvas : public QWidget {
 public:
  explicit BasicCanvas(QWidget* parent) : QWidget(parent) {}

  /// Copy what is needed out of @p values and schedule a repaint.
  virtual void setSamples(PJ::Span<const PJ_widget_value_t> values) = 0;

  /// Apply the complete settings object. Keys missing from it take their manifest default, so a
  /// setting the user cleared does not keep its previous value. On error the canvas is unchanged.
  virtual PJ::Status applySettings(const nlohmann::json& settings) = 0;
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

/// Reads settings keys with the manifest defaults. A key present with the wrong JSON type is an error.
PJ::Status readNumber(const nlohmann::json& settings, const char* key, double fallback, double& out);
PJ::Status readInteger(const nlohmann::json& settings, const char* key, int fallback, int min, int max, int& out);
PJ::Status readText(const nlohmann::json& settings, const char* key, const QString& fallback, QString& out);
/// The `decimals` setting every number-showing widget declares (0..12, as in the manifests).
PJ::Status readDecimals(const nlohmann::json& settings, int fallback, int& out);

/// A measurement with fixed decimals, or "--" when there is no sample.
QString formatNumber(std::optional<double> value, int decimals);

}  // namespace basic_widgets
