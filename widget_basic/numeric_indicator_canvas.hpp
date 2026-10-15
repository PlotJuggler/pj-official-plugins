#pragma once

#include "canvas.hpp"

namespace basic_widgets {

/// One number in large type, with an optional caption above it and a unit after it.
class NumericIndicatorCanvas : public NumberCanvas {
 public:
  explicit NumericIndicatorCanvas(QWidget* parent = nullptr);

  PJ::Status applySettings(const nlohmann::json& settings) override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  struct Settings {
    int decimals = 2;
    QString unit;
    QString label;
  };

  Settings settings_;
};

}  // namespace basic_widgets
