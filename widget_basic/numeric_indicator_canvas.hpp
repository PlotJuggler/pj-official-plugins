#pragma once

#include <string>

#include "canvas.hpp"

namespace basic_widgets {

/// One number in large type, with an optional caption above it and a unit after it.
class NumericIndicatorCanvas : public NumberCanvas {
 public:
  struct Settings {
    int decimals = 2;
    std::string unit;
    std::string label;
  };

  explicit NumericIndicatorCanvas(QWidget* parent = nullptr);

  static PJ::Expected<Settings> parseSettings(const nlohmann::json& settings);
  void applySettings(const Settings& settings);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  Settings settings_;
};

}  // namespace basic_widgets
