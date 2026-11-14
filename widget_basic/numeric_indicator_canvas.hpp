#pragma once

#include <QFont>
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
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  /// The caption depends only on the label and the widget's size and font, so it is fitted when
  /// one of those changes rather than on every paint.
  void updateCaptionFont();

  Settings settings_;
  QFont caption_font_;
};

}  // namespace basic_widgets
