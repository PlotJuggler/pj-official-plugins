#include "numeric_indicator_canvas.hpp"

#include <QPainter>
#include <algorithm>
#include <utility>

namespace basic_widgets {

NumericIndicatorCanvas::NumericIndicatorCanvas(QWidget* parent) : NumberCanvas(parent) {
  setMinimumSize(80, 40);
}

PJ::Expected<NumericIndicatorCanvas::Settings> NumericIndicatorCanvas::parseSettings(const nlohmann::json& settings) {
  Settings next;
  auto status = firstError(
      {readDecimals(settings, next.decimals), readText(settings, "unit", next.unit),
       readText(settings, "label", next.label)});
  if (!status) {
    return PJ::unexpected(std::move(status).error());
  }
  return next;
}

void NumericIndicatorCanvas::applySettings(const Settings& settings) {
  settings_ = settings;
  update();
}

void NumericIndicatorCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF area = QRectF(rect()).adjusted(8, 6, -8, -6);
  const QString label = QString::fromStdString(settings_.label);
  const QString unit = QString::fromStdString(settings_.unit);

  double caption_height = 0.0;
  if (!label.isEmpty()) {
    caption_height = std::clamp(area.height() * 0.22, 10.0, 20.0);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.setFont(fittedFont(font(), label, QSizeF(area.width(), caption_height), caption_height));
    painter.drawText(QRectF(area.left(), area.top(), area.width(), caption_height), Qt::AlignCenter, label);
  }

  const QRectF body(area.left(), area.top() + caption_height, area.width(), area.height() - caption_height);
  const QString number = formatNumber(value_, settings_.decimals);
  const QString text = unit.isEmpty() || !value_ ? number : number + " " + unit;
  painter.setPen(palette().color(QPalette::WindowText));
  painter.setFont(fittedFont(font(), text, body.size(), body.height() * 0.8));
  painter.drawText(body, Qt::AlignCenter, text);
}

}  // namespace basic_widgets
