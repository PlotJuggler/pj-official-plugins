#pragma once

#include <QColor>
#include <optional>
#include <string>

#include "canvas.hpp"

class QPainter;

namespace basic_widgets {

/// Single-value gauge, drawn as a 270-degree arc or as a horizontal bar. The part of the scale from
/// `red_from` upwards is marked and the fill turns red once the value reaches it.
class GaugeCanvas : public NumberCanvas {
 public:
  struct Settings {
    double min = 0.0;
    double max = 100.0;
    double red_from = 80.0;
    std::string unit;
    int decimals = 2;
    bool arc = true;
  };

  explicit GaugeCanvas(QWidget* parent = nullptr);

  static PJ::Expected<Settings> parseSettings(const nlohmann::json& settings);
  void applySettings(const Settings& settings);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  void paintArc(QPainter& painter, double fraction, const QString& text) const;
  void paintBar(QPainter& painter, double fraction, const QString& text) const;
  /// The tint of the scale from `red_from` up; nullopt when that threshold lies outside (min, max).
  [[nodiscard]] std::optional<QColor> redZone() const;
  [[nodiscard]] double fractionOf(double value) const;

  Settings settings_;
};

}  // namespace basic_widgets
