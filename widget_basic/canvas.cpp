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
  constexpr int kMinPixelSize = 6;
  const int max_size = static_cast<int>(std::max(static_cast<double>(kMinPixelSize), max_pixel_size));
  QFont font = base;
  font.setPixelSize(max_size);
  if (max_size <= kMinPixelSize) {
    return font;
  }
  const auto fits = [&](int pixel_size) {
    font.setPixelSize(pixel_size);
    const QFontMetricsF metrics(font);
    return metrics.horizontalAdvance(text) <= box.width() && metrics.height() <= box.height();
  };

  // Measure once at the largest size. Text extent grows close to linearly with the pixel size, so
  // that one measurement gives a near-exact estimate. Hinting makes it only near-exact, so the
  // estimate is then corrected by a step or two in whichever direction it is off.
  const QFontMetricsF metrics(font);
  const double width = metrics.horizontalAdvance(text);
  const double height = metrics.height();
  if (width <= box.width() && height <= box.height()) {
    return font;
  }
  double scale = height > 0.0 ? box.height() / height : 1.0;
  if (width > 0.0) {
    scale = std::min(scale, box.width() / width);
  }
  int size = static_cast<int>(
      std::clamp(scale * max_size, static_cast<double>(kMinPixelSize), static_cast<double>(max_size - 1)));
  if (fits(size)) {
    while (size + 1 < max_size && fits(size + 1)) {
      ++size;
    }
  } else {
    while (size > kMinPixelSize && !fits(--size)) {}
  }
  font.setPixelSize(size);
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
