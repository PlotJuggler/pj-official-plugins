#include "value_table_canvas.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace basic_widgets {

ValueTableCanvas::ValueTableCanvas(QWidget* parent) : BasicCanvas(parent) {
  setMinimumSize(120, 40);
}

void ValueTableCanvas::setSamples(PJ::Span<const PJ_widget_value_t> values) {
  rows_.clear();
  for (const auto& value : values) {
    if (value.input != 0) {
      continue;
    }
    Row row;
    row.slot = value.slot;
    // The label pointer dies with the call: copy it now, like the number.
    row.label = value.label.data != nullptr && value.label.size > 0
                    ? QString::fromUtf8(value.label.data, static_cast<qsizetype>(value.label.size))
                    : QStringLiteral("Series %1").arg(value.slot + 1);
    if (value.kind == PJ_WIDGET_VALUE_NUMBER && std::isfinite(value.number)) {
      row.value = value.number;
    }
    rows_.push_back(row);
  }
  std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.slot < b.slot; });
  update();
}

PJ::Status ValueTableCanvas::applySettings(const nlohmann::json& settings) {
  int decimals = 3;
  if (auto status = readInteger(settings, "decimals", 3, 0, 12, decimals); !status) {
    return status;
  }
  decimals_ = decimals;
  update();
  return PJ::okStatus();
}

void ValueTableCanvas::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QFontMetrics metrics(font());
  const int row_height = metrics.height() + 8;
  const QRect area = rect().adjusted(4, 4, -4, -4);

  if (rows_.empty()) {
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(area, Qt::AlignCenter, QStringLiteral("No series"));
    return;
  }

  painter.setClipRect(area);
  int y = area.top();
  bool odd = false;
  for (const Row& row : rows_) {
    if (y > area.bottom()) {
      break;
    }
    const QRect cell(area.left(), y, area.width(), row_height);
    if (odd) {
      painter.fillRect(cell, palette().color(QPalette::AlternateBase));
    }
    odd = !odd;
    const QRect text_cell = cell.adjusted(6, 0, -6, 0);
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(
        text_cell, Qt::AlignLeft | Qt::AlignVCenter,
        metrics.elidedText(row.label, Qt::ElideMiddle, text_cell.width() * 2 / 3));
    painter.drawText(
        text_cell, Qt::AlignRight | Qt::AlignVCenter,
        row.value ? QString::number(*row.value, 'f', decimals_) : QStringLiteral("--"));
    y += row_height;
  }
}

}  // namespace basic_widgets
