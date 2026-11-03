#include "gauge_canvas.hpp"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <utility>

namespace basic_widgets {

namespace {
constexpr double kArcStartDegrees = 225.0;
constexpr double kArcSpanDegrees = 270.0;
constexpr int kZoneAlpha = 110;
constexpr int kSixteenth = 16;  // QPainter::drawArc takes angles in 1/16 degree
}  // namespace

GaugeCanvas::GaugeCanvas(QWidget* parent) : NumberCanvas(parent) {
  setMinimumSize(80, 60);
}

PJ::Expected<GaugeCanvas::Settings> GaugeCanvas::parseSettings(const nlohmann::json& settings) {
  Settings next;
  std::string mode = next.arc ? "arc" : "bar";
  auto status = firstError(
      {readNumber(settings, "min", next.min), readNumber(settings, "max", next.max),
       readNumber(settings, "red_from", next.red_from), readText(settings, "unit", next.unit),
       readDecimals(settings, next.decimals), readText(settings, "mode", mode)});
  if (!status) {
    return PJ::unexpected(std::move(status).error());
  }
  if (mode != "arc" && mode != "bar") {
    return PJ::unexpected("setting \"mode\" must be \"arc\" or \"bar\"");
  }
  next.arc = mode == "arc";
  return next;
}

void GaugeCanvas::applySettings(const Settings& settings) {
  settings_ = settings;
  update();
}

std::optional<QColor> GaugeCanvas::redZone() const {
  if (!(settings_.red_from > settings_.min && settings_.red_from < settings_.max)) {
    return std::nullopt;
  }
  QColor zone = badColor();
  zone.setAlpha(kZoneAlpha);
  return zone;
}

double GaugeCanvas::fractionOf(double value) const {
  const double span = settings_.max - settings_.min;
  if (!(span > 0.0)) {
    return 0.0;
  }
  return std::clamp((value - settings_.min) / span, 0.0, 1.0);
}

void GaugeCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QString text = formatNumber(value_, settings_.decimals) +
                       (value_ && !settings_.unit.empty() ? " " + QString::fromStdString(settings_.unit) : QString());
  const double fraction = value_ ? fractionOf(*value_) : 0.0;
  if (settings_.arc) {
    paintArc(painter, fraction, text);
  } else {
    paintBar(painter, fraction, text);
  }
}

void GaugeCanvas::paintArc(QPainter& painter, double fraction, const QString& text) const {
  const QRectF area = QRectF(rect()).adjusted(8, 8, -8, -8);
  const double side = std::min(area.width(), area.height());
  const double thickness = std::max(4.0, side * 0.1);
  const QRectF arc_rect(
      area.center().x() - side / 2.0 + thickness / 2.0, area.center().y() - side / 2.0 + thickness / 2.0,
      side - thickness, side - thickness);

  QPen pen(palette().color(QPalette::Mid), thickness, Qt::SolidLine, Qt::FlatCap);
  painter.setPen(pen);
  painter.drawArc(
      arc_rect, static_cast<int>(kArcStartDegrees * kSixteenth), static_cast<int>(-kArcSpanDegrees * kSixteenth));

  if (const auto zone = redZone()) {
    const double red_fraction = fractionOf(settings_.red_from);
    pen.setColor(*zone);
    painter.setPen(pen);
    painter.drawArc(
        arc_rect, static_cast<int>((kArcStartDegrees - kArcSpanDegrees * red_fraction) * kSixteenth),
        static_cast<int>(-kArcSpanDegrees * (1.0 - red_fraction) * kSixteenth));
  }

  if (value_ && fraction > 0.0) {
    pen.setColor(*value_ >= settings_.red_from ? badColor() : palette().color(QPalette::Highlight));
    painter.setPen(pen);
    painter.drawArc(
        arc_rect, static_cast<int>(kArcStartDegrees * kSixteenth),
        static_cast<int>(-kArcSpanDegrees * fraction * kSixteenth));
  }

  painter.setPen(palette().color(QPalette::WindowText));
  const double inner = side - 2.0 * thickness;
  painter.setFont(fittedFont(font(), text, QSizeF(inner * 0.9, inner * 0.3), inner * 0.3));
  painter.drawText(
      QRectF(area.center().x() - inner / 2.0, area.center().y() - inner * 0.2, inner, inner * 0.4), Qt::AlignCenter,
      text);

  QFont small = font();
  small.setPixelSize(static_cast<int>(std::max(8.0, side * 0.07)));
  painter.setFont(small);
  painter.setPen(palette().color(QPalette::PlaceholderText));
  const double label_w = side * 0.35;
  const double label_y = area.center().y() + side / 2.0 - small.pixelSize() * 1.4;
  painter.drawText(
      QRectF(area.center().x() - side / 2.0, label_y, label_w, small.pixelSize() * 1.4),
      Qt::AlignLeft | Qt::AlignVCenter, QString::number(settings_.min, 'g', 4));
  painter.drawText(
      QRectF(area.center().x() + side / 2.0 - label_w, label_y, label_w, small.pixelSize() * 1.4),
      Qt::AlignRight | Qt::AlignVCenter, QString::number(settings_.max, 'g', 4));
}

void GaugeCanvas::paintBar(QPainter& painter, double fraction, const QString& text) const {
  const QRectF area = QRectF(rect()).adjusted(10, 8, -10, -8);
  const double bar_height = std::clamp(area.height() * 0.22, 6.0, 40.0);
  const QRectF track(area.left(), area.center().y() - bar_height / 2.0, area.width(), bar_height);
  const double radius = bar_height / 2.0;

  painter.setPen(Qt::NoPen);
  painter.setBrush(palette().color(QPalette::Mid));
  painter.drawRoundedRect(track, radius, radius);

  if (const auto zone = redZone()) {
    const double x = track.left() + track.width() * fractionOf(settings_.red_from);
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(track, radius, radius);
    painter.setClipPath(clip);
    painter.fillRect(QRectF(x, track.top(), track.right() - x, track.height()), *zone);
    painter.restore();
  }

  if (value_ && fraction > 0.0) {
    painter.setBrush(*value_ >= settings_.red_from ? badColor() : palette().color(QPalette::Highlight));
    painter.drawRoundedRect(
        QRectF(track.left(), track.top(), std::max(bar_height, track.width() * fraction), bar_height), radius, radius);
  }

  painter.setPen(palette().color(QPalette::WindowText));
  const double text_height = std::max(10.0, track.top() - area.top());
  painter.setFont(fittedFont(font(), text, QSizeF(area.width(), text_height), text_height));
  painter.drawText(QRectF(area.left(), area.top(), area.width(), text_height), Qt::AlignCenter, text);

  QFont small = font();
  small.setPixelSize(static_cast<int>(std::max(8.0, std::min(area.height() * 0.14, 14.0))));
  painter.setFont(small);
  painter.setPen(palette().color(QPalette::PlaceholderText));
  const QRectF labels(area.left(), track.bottom() + 2.0, area.width(), small.pixelSize() * 1.4);
  painter.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter, QString::number(settings_.min, 'g', 4));
  painter.drawText(labels, Qt::AlignRight | Qt::AlignVCenter, QString::number(settings_.max, 'g', 4));
}

}  // namespace basic_widgets
