#pragma once

#include "canvas.hpp"

namespace basic_widgets {

/// Three lamps. Green below `green_below`, red from `red_from` upwards, amber in between; all lamps are
/// dark while there is no sample. `red_from` wins when the two thresholds are inverted.
class TrafficLightCanvas : public NumberCanvas {
 public:
  struct Settings {
    double green_below = 50.0;
    double red_from = 80.0;
  };

  explicit TrafficLightCanvas(QWidget* parent = nullptr);

  static PJ::Expected<Settings> parseSettings(const nlohmann::json& settings);
  void applySettings(const Settings& settings);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  Settings settings_;
};

}  // namespace basic_widgets
