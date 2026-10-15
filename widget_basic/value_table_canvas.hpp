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
  explicit ValueTableCanvas(QWidget* parent = nullptr);

  void setSamples(PJ::Span<const PJ_widget_value_t> values) override;
  PJ::Status applySettings(const nlohmann::json& settings) override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  struct Row {
    uint32_t slot = 0;
    QString label;
    std::optional<double> value;
  };

  int decimals_ = 3;
  std::vector<Row> rows_;
};

}  // namespace basic_widgets
