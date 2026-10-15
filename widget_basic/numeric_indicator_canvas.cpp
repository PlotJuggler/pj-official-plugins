#include "numeric_indicator_canvas.hpp"

#include <QPainter>
#include <algorithm>

namespace basic_widgets {

NumericIndicatorCanvas::NumericIndicatorCanvas(QWidget* parent) : NumberCanvas(parent) {
  setMinimumSize(80, 40);
}

PJ::Status NumericIndicatorCanvas::applySettings(const nlohmann::json& settings) {
  Settings next;
  if (auto status = readInteger(settings, "decimals", 2, 0, 12, next.decimals); !status) {
    return status;
  }
  if (auto status = readText(settings, "unit", QString(), next.unit); !status) {
    return status;
  }
  if (auto status = readText(settings, "label", QString(), next.label); !status) {
    return status;
  }
  settings_ = next;
  update();
  return PJ::okStatus();
}

void NumericIndicatorCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF area = QRectF(rect()).adjusted(8, 6, -8, -6);

  double caption_height = 0.0;
  if (!settings_.label.isEmpty()) {
    caption_height = std::clamp(area.height() * 0.22, 10.0, 20.0);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.setFont(fittedFont(font(), settings_.label, QSizeF(area.width(), caption_height), caption_height));
    painter.drawText(QRectF(area.left(), area.top(), area.width(), caption_height), Qt::AlignCenter, settings_.label);
  }

  const QRectF body(area.left(), area.top() + caption_height, area.width(), area.height() - caption_height);
  const QString number = value_ ? QString::number(*value_, 'f', settings_.decimals) : QStringLiteral("--");
  const QString text = settings_.unit.isEmpty() || !value_ ? number : number + " " + settings_.unit;
  painter.setPen(palette().color(QPalette::WindowText));
  painter.setFont(fittedFont(font(), text, body.size(), body.height() * 0.8));
  painter.drawText(body, Qt::AlignCenter, text);
}

}  // namespace basic_widgets
