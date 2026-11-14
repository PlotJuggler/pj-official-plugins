#include "numeric_indicator_canvas.hpp"

#include <QEvent>
#include <QPainter>
#include <algorithm>
#include <utility>

namespace basic_widgets {

namespace {

QRectF contentArea(const QRect& rect) {
  return QRectF(rect).adjusted(8, 6, -8, -6);
}

double captionHeight(const QRectF& area) {
  return std::clamp(area.height() * 0.22, 10.0, 20.0);
}

}  // namespace

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
  updateCaptionFont();
  update();
}

void NumericIndicatorCanvas::resizeEvent(QResizeEvent* event) {
  NumberCanvas::resizeEvent(event);
  updateCaptionFont();
}

void NumericIndicatorCanvas::changeEvent(QEvent* event) {
  NumberCanvas::changeEvent(event);
  if (event->type() == QEvent::FontChange) {
    updateCaptionFont();
  }
}

void NumericIndicatorCanvas::updateCaptionFont() {
  const QString label = QString::fromStdString(settings_.label);
  if (label.isEmpty()) {
    return;
  }
  const QRectF area = contentArea(rect());
  const double caption_height = captionHeight(area);
  caption_font_ = fittedFont(font(), label, QSizeF(area.width(), caption_height), caption_height);
}

void NumericIndicatorCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF area = contentArea(rect());
  const QString label = QString::fromStdString(settings_.label);
  const QString unit = QString::fromStdString(settings_.unit);

  double caption_height = 0.0;
  if (!label.isEmpty()) {
    caption_height = captionHeight(area);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.setFont(caption_font_);
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
