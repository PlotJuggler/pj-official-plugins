#include "traffic_light_canvas.hpp"

#include <QPainter>
#include <algorithm>
#include <utility>

namespace basic_widgets {

TrafficLightCanvas::TrafficLightCanvas(QWidget* parent) : NumberCanvas(parent) {
  setMinimumSize(40, 40);
}

PJ::Expected<TrafficLightCanvas::Settings> TrafficLightCanvas::parseSettings(const nlohmann::json& settings) {
  Settings next;
  auto status = firstError(
      {readNumber(settings, "green_below", next.green_below), readNumber(settings, "red_from", next.red_from)});
  if (!status) {
    return PJ::unexpected(std::move(status).error());
  }
  return next;
}

void TrafficLightCanvas::applySettings(const Settings& settings) {
  settings_ = settings;
  update();
}

void TrafficLightCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  // Index of the lit lamp: 0 green, 1 amber, 2 red, -1 none.
  int lit = -1;
  if (value_) {
    lit = *value_ >= settings_.red_from ? 2 : (*value_ < settings_.green_below ? 0 : 1);
  }
  const QColor lamps[3] = {goodColor(), warnColor(), badColor()};

  const QRectF area = QRectF(rect()).adjusted(6, 6, -6, -6);
  const bool horizontal = area.width() > area.height() * 1.6;
  const double diameter =
      horizontal ? std::min(area.height(), area.width() / 3.4) : std::min(area.width(), area.height() / 3.4);
  const double gap = diameter * 0.2;
  const double total = diameter * 3.0 + gap * 2.0;
  const QPointF origin = horizontal ? QPointF(area.center().x() - total / 2.0, area.center().y() - diameter / 2.0)
                                    : QPointF(area.center().x() - diameter / 2.0, area.center().y() - total / 2.0);

  const QColor off = palette().color(QPalette::Mid);
  for (int i = 0; i < 3; ++i) {
    const double offset = (diameter + gap) * i;
    // Vertical order is red on top, as on a signal; horizontal order is green to red.
    const int lamp = horizontal ? i : 2 - i;
    const QRectF cell(
        horizontal ? origin.x() + offset : origin.x(), horizontal ? origin.y() : origin.y() + offset, diameter,
        diameter);
    painter.setPen(QPen(palette().color(QPalette::WindowText), 1.0));
    painter.setBrush(lamp == lit ? lamps[lamp] : off);
    painter.drawEllipse(cell);
  }
}

}  // namespace basic_widgets
