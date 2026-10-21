#pragma once

#include <QString>
#include <optional>
#include <vector>

#include "canvas.hpp"

namespace basic_widgets {

/// One row per wired series, in slot order, labelled with the series name the host puts in the
/// value's `label` ("Series N" when the host gives none).
class ValueTableCanvas : public BasicCanvas {
 public:
  struct Settings {
    int decimals = 3;
  };

  explicit ValueTableCanvas(QWidget* parent = nullptr);

  void setSamples(PJ::Span<const PJ_widget_value_t> values) override;

  static PJ::Expected<Settings> parseSettings(const nlohmann::json& settings);
  void applySettings(const Settings& settings);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  struct Row {
    uint32_t slot = 0;
    QString label;
    std::optional<double> value;
  };

  Settings settings_;
  std::vector<Row> rows_;
};

}  // namespace basic_widgets
