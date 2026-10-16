#include "canvas.hpp"

#include <QFontMetricsF>
#include <algorithm>
#include <cmath>
#include <string>

namespace basic_widgets {

QColor goodColor() {
  return QColor(46, 160, 67);
}

QColor warnColor() {
  return QColor(224, 160, 16);
}

QColor badColor() {
  return QColor(214, 56, 56);
}

void NumberCanvas::setSamples(PJ::Span<const PJ_widget_value_t> values) {
  value_ = numberAt(values, 0);
  update();
}

std::optional<double> numberAt(PJ::Span<const PJ_widget_value_t> values, uint32_t input) {
  for (const auto& value : values) {
    if (value.input == input && value.kind == PJ_WIDGET_VALUE_NUMBER && std::isfinite(value.number)) {
      return value.number;
    }
  }
  return std::nullopt;
}

QFont fittedFont(const QFont& base, const QString& text, const QSizeF& box, double max_pixel_size) {
  QFont font = base;
  double size = std::max(6.0, max_pixel_size);
  for (; size > 6.0; size -= 1.0) {
    font.setPixelSize(static_cast<int>(size));
    const QFontMetricsF metrics(font);
    if (metrics.horizontalAdvance(text) <= box.width() && metrics.height() <= box.height()) {
      return font;
    }
  }
  font.setPixelSize(6);
  return font;
}

PJ::Status readNumber(const nlohmann::json& settings, const char* key, double& out) {
  const auto it = settings.find(key);
  if (it == settings.end() || it->is_null()) {
    return PJ::okStatus();
  }
  if (!it->is_number()) {
    return PJ::unexpected(std::string("setting \"") + key + "\" must be a number");
  }
  out = it->get<double>();
  return PJ::okStatus();
}

PJ::Status readInteger(const nlohmann::json& settings, const char* key, int min, int max, int& out) {
  double value = out;
  if (auto status = readNumber(settings, key, value); !status) {
    return status;
  }
  out = static_cast<int>(std::clamp(std::lround(value), static_cast<long>(min), static_cast<long>(max)));
  return PJ::okStatus();
}

PJ::Status readDecimals(const nlohmann::json& settings, int& out) {
  return readInteger(settings, "decimals", 0, 12, out);
}

PJ::Status firstError(std::initializer_list<PJ::Status> statuses) {
  for (const auto& status : statuses) {
    if (!status) {
      return status;
    }
  }
  return PJ::okStatus();
}

QString formatNumber(std::optional<double> value, int decimals) {
  return value ? QString::number(*value, 'f', decimals) : QStringLiteral("--");
}

PJ::Status readText(const nlohmann::json& settings, const char* key, std::string& out) {
  const auto it = settings.find(key);
  if (it == settings.end() || it->is_null()) {
    return PJ::okStatus();
  }
  if (!it->is_string()) {
    return PJ::unexpected(std::string("setting \"") + key + "\" must be text");
  }
  out = it->get<std::string>();
  return PJ::okStatus();
}

}  // namespace basic_widgets
