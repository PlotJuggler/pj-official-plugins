#include "timestamp_parsing.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "date/date.h"

namespace PJ::CSV {

std::string Trim(const std::string& str) {
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}

std::optional<double> toDouble(const std::string& str) {
  if (str.empty()) return std::nullopt;

  // Handle European comma decimal separator by replacing with dot.
  // Only replace if there's exactly one comma and no dots (to avoid
  // misinterpreting thousand separators).
  std::string normalized = str;
  auto comma_count = std::count(normalized.begin(), normalized.end(), ',');
  auto dot_count = std::count(normalized.begin(), normalized.end(), '.');
  if (comma_count == 1 && dot_count == 0) {
    std::replace(normalized.begin(), normalized.end(), ',', '.');
  }

  const char* start = normalized.c_str();
  char* end = nullptr;
  errno = 0;
  double value = std::strtod(start, &end);
  if (errno != 0 || end == start || *end != '\0') {
    return std::nullopt;
  }
  return value;
}

static const char* const UNAMBIGUOUS_FORMATS[] = {
    "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%dT%H:%M:%SZ",
    "%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M:%S%z", "%Y-%m-%d",
    "%Y/%m/%d %H:%M:%S",
};
static constexpr size_t NUM_UNAMBIGUOUS_FORMATS =
    sizeof(UNAMBIGUOUS_FORMATS) / sizeof(UNAMBIGUOUS_FORMATS[0]);

static constexpr int64_t EPOCH_FIRST = 1400000000;
static constexpr int64_t EPOCH_LAST = 2000000000;

static std::pair<std::string, std::chrono::nanoseconds> ExtractFractionalSeconds(
    const std::string& input) {
  size_t dot_pos = input.rfind('.');
  size_t colon_pos = input.rfind(':');

  if (dot_pos == std::string::npos || colon_pos == std::string::npos || dot_pos <= colon_pos) {
    return {input, std::chrono::nanoseconds{0}};
  }

  size_t frac_start = dot_pos + 1;
  size_t frac_end = frac_start;

  while (frac_end < input.size() && std::isdigit(static_cast<unsigned char>(input[frac_end]))) {
    frac_end++;
  }

  if (frac_end <= frac_start) {
    return {input, std::chrono::nanoseconds{0}};
  }

  std::string frac_str = input.substr(frac_start, frac_end - frac_start);

  if (frac_str.size() < 9) {
    frac_str.append(9 - frac_str.size(), '0');
  } else if (frac_str.size() > 9) {
    frac_str = frac_str.substr(0, 9);
  }

  std::chrono::nanoseconds fractional_ns{0};
  try {
    fractional_ns = std::chrono::nanoseconds{std::stoll(frac_str)};
  } catch (...) {
  }

  std::string base_str = input.substr(0, dot_pos) + input.substr(frac_end);
  return {base_str, fractional_ns};
}

static std::optional<double> TryParseFormat(const std::string& base_str, const char* fmt,
                                            std::chrono::nanoseconds fractional_ns) {
  std::istringstream in{base_str};
  in.imbue(std::locale::classic());

  date::sys_time<std::chrono::seconds> tp;
  in >> date::parse(fmt, tp);

  if (!in.fail()) {
    auto duration = tp.time_since_epoch();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration) + fractional_ns;
    return std::chrono::duration<double>(total_ns).count();
  }
  return std::nullopt;
}

struct NumericInfo {
  bool is_number = false;
  bool has_decimal = false;
  bool has_exponent = false;
};

static NumericInfo CheckNumeric(const std::string& str) {
  NumericInfo info;
  info.is_number = true;

  bool has_digit = false;

  for (size_t i = 0; i < str.size(); i++) {
    const char c = str[i];

    if (c == 'e' || c == 'E') {
      if (info.has_exponent) {
        info.is_number = false;
        break;
      }
      info.has_exponent = true;
      continue;
    }

    if (c == '-' || c == '+') {
      if (i == 0 || str[i - 1] == 'e' || str[i - 1] == 'E') {
        continue;
      }
      info.is_number = false;
      break;
    }

    if (c == '.' || c == ',') {
      if (info.has_decimal || info.has_exponent) {
        info.is_number = false;
        break;
      }
      info.has_decimal = true;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c))) {
      has_digit = true;
    } else {
      info.is_number = false;
      break;
    }
  }

  if (info.is_number) {
    const char last = str.back();
    if (!has_digit || last == 'e' || last == 'E' || last == '+' || last == '-') {
      info.is_number = false;
    }
  }

  return info;
}

static ColumnType DetectEpochType(int64_t ts) {
  if (ts > EPOCH_FIRST * 1000000000LL && ts < EPOCH_LAST * 1000000000LL) {
    return ColumnType::EPOCH_NANOS;
  }
  if (ts > EPOCH_FIRST * 1000000LL && ts < EPOCH_LAST * 1000000LL) {
    return ColumnType::EPOCH_MICROS;
  }
  if (ts > EPOCH_FIRST * 1000LL && ts < EPOCH_LAST * 1000LL) {
    return ColumnType::EPOCH_MILLIS;
  }
  if (ts > EPOCH_FIRST && ts < EPOCH_LAST) {
    return ColumnType::EPOCH_SECONDS;
  }
  return ColumnType::NUMBER;
}

static double EpochToSeconds(int64_t ts, ColumnType type) {
  switch (type) {
    case ColumnType::EPOCH_NANOS:
      return static_cast<double>(ts) * 1e-9;
    case ColumnType::EPOCH_MICROS:
      return static_cast<double>(ts) * 1e-6;
    case ColumnType::EPOCH_MILLIS:
      return static_cast<double>(ts) * 1e-3;
    case ColumnType::EPOCH_SECONDS:
    default:
      return static_cast<double>(ts);
  }
}

bool IsDayFirstFormat(const std::string& str, char separator) {
  int first = 0;
  int second = 0;
  size_t pos = 0;

  while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos]))) {
    first = first * 10 + (str[pos] - '0');
    pos++;
  }

  if (pos < str.size() && str[pos] == separator) {
    pos++;
  }

  while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos]))) {
    second = second * 10 + (str[pos] - '0');
    pos++;
  }

  if (first > 12 && first <= 31) {
    return true;
  }
  if (second > 12 && second <= 31) {
    return false;
  }

  return true;
}

std::optional<double> AutoParseTimestamp(const std::string& str) {
  std::string trimmed = Trim(str);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  auto num_info = CheckNumeric(trimmed);
  if (num_info.is_number) {
    try {
      if (!num_info.has_decimal && !num_info.has_exponent) {
        int64_t ts = std::stoll(trimmed);
        ColumnType epoch_type = DetectEpochType(ts);
        return EpochToSeconds(ts, epoch_type);
      }
      return toDouble(trimmed);
    } catch (...) {
    }
  }

  auto [base_str, fractional_ns] = ExtractFractionalSeconds(trimmed);

  for (size_t i = 0; i < NUM_UNAMBIGUOUS_FORMATS; i++) {
    if (auto result = TryParseFormat(base_str, UNAMBIGUOUS_FORMATS[i], fractional_ns)) {
      return result;
    }
  }

  if (base_str.find('/') != std::string::npos && !base_str.empty() && base_str[0] != '2') {
    const char* fmt =
        IsDayFirstFormat(base_str, '/') ? "%d/%m/%Y %H:%M:%S" : "%m/%d/%Y %H:%M:%S";
    if (auto result = TryParseFormat(base_str, fmt, fractional_ns)) {
      return result;
    }
  }

  if (base_str.find('-') != std::string::npos && !base_str.empty() && base_str[0] != '2') {
    if (IsDayFirstFormat(base_str, '-')) {
      if (auto result = TryParseFormat(base_str, "%d-%m-%Y %H:%M:%S", fractional_ns)) {
        return result;
      }
    }
  }

  return std::nullopt;
}

std::optional<double> FormatParseTimestamp(const std::string& str, const std::string& format) {
  std::string trimmed = Trim(str);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  std::string adjusted_format = format;
  std::chrono::nanoseconds fractional_ns{0};
  std::string adjusted_input = trimmed;

  size_t zzz_pos = format.find(".zzz");
  if (zzz_pos != std::string::npos) {
    size_t z_count = 0;
    size_t z_start = zzz_pos + 1;
    while (z_start + z_count < format.size() && format[z_start + z_count] == 'z') {
      z_count++;
    }

    size_t input_dot_pos = adjusted_input.find('.', zzz_pos > 0 ? zzz_pos - 5 : 0);
    if (input_dot_pos != std::string::npos) {
      auto [base, frac] = ExtractFractionalSeconds(adjusted_input);
      fractional_ns = frac;
      adjusted_input = base;
      adjusted_format = format.substr(0, zzz_pos) + format.substr(z_start + z_count);
    }
  }

  std::string strptime_format = adjusted_format;
  auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.length(), to);
      pos += to.length();
    }
  };

  replace_all(strptime_format, "yyyy", "%Y");
  replace_all(strptime_format, "yy", "%y");
  replace_all(strptime_format, "MM", "%m");
  replace_all(strptime_format, "dd", "%d");
  replace_all(strptime_format, "hh", "%H");
  replace_all(strptime_format, "HH", "%H");
  replace_all(strptime_format, "mm", "%M");
  replace_all(strptime_format, "ss", "%S");

  return TryParseFormat(adjusted_input, strptime_format.c_str(), fractional_ns);
}

ColumnTypeInfo DetectColumnType(const std::string& str) {
  ColumnTypeInfo info;

  std::string trimmed = Trim(str);
  if (trimmed.empty()) {
    info.type = ColumnType::STRING;
    return info;
  }

  if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
    info.type = ColumnType::HEX;
    return info;
  }

  auto num_info = CheckNumeric(trimmed);
  if (num_info.is_number) {
    if (num_info.has_decimal) {
      info.type = ColumnType::NUMBER;
      return info;
    }

    try {
      int64_t ts = std::stoll(trimmed);
      info.type = DetectEpochType(ts);
      return info;
    } catch (...) {
    }

    info.type = ColumnType::NUMBER;
    return info;
  }

  auto [base_str, fractional_ns] = ExtractFractionalSeconds(trimmed);
  info.has_fractional = (fractional_ns.count() > 0 || trimmed != base_str);

  auto try_format = [&](const char* fmt) -> bool {
    std::istringstream in{base_str};
    in.imbue(std::locale::classic());
    date::sys_time<std::chrono::seconds> tp;
    in >> date::parse(fmt, tp);
    return !in.fail();
  };

  bool has_slash = (trimmed.find('/') != std::string::npos);
  bool has_dash = (trimmed.find('-') != std::string::npos);
  bool has_colon = (trimmed.find(':') != std::string::npos);

  if ((has_slash || has_dash) && !has_colon) {
    auto try_date_format = [&](const char* fmt) -> bool {
      std::istringstream in{trimmed};
      in.imbue(std::locale::classic());
      date::year_month_day ymd;
      in >> date::parse(fmt, ymd);
      return !in.fail() && ymd.ok() && (in.peek() == EOF);
    };

    const char* date_formats[] = {"%Y-%m-%d", "%Y/%m/%d", "%d/%m/%Y",
                                  "%m/%d/%Y", "%d-%m-%Y", "%m-%d-%Y"};
    for (const char* fmt : date_formats) {
      if (try_date_format(fmt)) {
        info.type = ColumnType::DATE_ONLY;
        info.format = fmt;
        return info;
      }
    }
  }

  if (has_colon && !has_slash && !has_dash) {
    auto try_time_format = [&](const char* fmt) -> bool {
      std::istringstream in{base_str};
      in.imbue(std::locale::classic());
      std::chrono::seconds tod;
      in >> date::parse(fmt, tod);
      return !in.fail();
    };

    const char* time_formats[] = {"%H:%M:%S", "%I:%M:%S %p"};
    for (const char* fmt : time_formats) {
      if (try_time_format(fmt)) {
        info.type = ColumnType::TIME_ONLY;
        info.format = fmt;
        return info;
      }
    }
  }

  for (size_t i = 0; i < NUM_UNAMBIGUOUS_FORMATS; i++) {
    if (try_format(UNAMBIGUOUS_FORMATS[i])) {
      info.type = ColumnType::DATETIME;
      info.format = UNAMBIGUOUS_FORMATS[i];
      return info;
    }
  }

  if (base_str.find('/') != std::string::npos && !base_str.empty() && base_str[0] != '2') {
    const char* fmt =
        IsDayFirstFormat(base_str, '/') ? "%d/%m/%Y %H:%M:%S" : "%m/%d/%Y %H:%M:%S";
    if (try_format(fmt)) {
      info.type = ColumnType::DATETIME;
      info.format = fmt;
      return info;
    }
  }

  if (base_str.find('-') != std::string::npos && !base_str.empty() && base_str[0] != '2') {
    if (IsDayFirstFormat(base_str, '-')) {
      const char* fmt = "%d-%m-%Y %H:%M:%S";
      if (try_format(fmt)) {
        info.type = ColumnType::DATETIME;
        info.format = fmt;
        return info;
      }
    }
  }

  info.type = ColumnType::STRING;
  return info;
}

std::optional<double> ParseWithType(const std::string& str, const ColumnTypeInfo& type_info) {
  std::string trimmed = Trim(str);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  try {
    switch (type_info.type) {
      case ColumnType::NUMBER:
        return toDouble(trimmed);

      case ColumnType::HEX:
        return static_cast<double>(std::stoll(trimmed, nullptr, 16));

      case ColumnType::EPOCH_SECONDS:
      case ColumnType::EPOCH_MILLIS:
      case ColumnType::EPOCH_MICROS:
      case ColumnType::EPOCH_NANOS:
        return EpochToSeconds(std::stoll(trimmed), type_info.type);

      case ColumnType::DATETIME: {
        auto [base_str, fractional_ns] = ExtractFractionalSeconds(trimmed);
        if (!type_info.has_fractional) {
          fractional_ns = std::chrono::nanoseconds{0};
        }
        return TryParseFormat(base_str, type_info.format.c_str(), fractional_ns);
      }

      case ColumnType::DATE_ONLY: {
        std::istringstream in{trimmed};
        in.imbue(std::locale::classic());
        date::year_month_day ymd;
        in >> date::parse(type_info.format.c_str(), ymd);
        if (!in.fail() && ymd.ok()) {
          auto tp = date::sys_days{ymd};
          return std::chrono::duration<double>(tp.time_since_epoch()).count();
        }
        return std::nullopt;
      }

      case ColumnType::TIME_ONLY: {
        auto [base_str, fractional_ns] = ExtractFractionalSeconds(trimmed);
        if (!type_info.has_fractional) {
          fractional_ns = std::chrono::nanoseconds{0};
        }
        std::istringstream in{base_str};
        in.imbue(std::locale::classic());
        std::chrono::seconds tod;
        in >> date::parse(type_info.format.c_str(), tod);
        if (!in.fail()) {
          auto total_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(tod) + fractional_ns;
          return std::chrono::duration<double>(total_ns).count();
        }
        return std::nullopt;
      }

      case ColumnType::STRING:
      default:
        return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> ParseCombinedDateTime(const std::string& date_str,
                                            const std::string& time_str,
                                            const ColumnTypeInfo& date_info,
                                            const ColumnTypeInfo& time_info) {
  std::string trimmed_date = Trim(date_str);
  std::string trimmed_time = Trim(time_str);

  if (trimmed_date.empty() || trimmed_time.empty()) {
    return std::nullopt;
  }

  try {
    std::istringstream date_in{trimmed_date};
    date_in.imbue(std::locale::classic());
    date::year_month_day ymd;
    date_in >> date::parse(date_info.format.c_str(), ymd);
    if (date_in.fail() || !ymd.ok()) {
      return std::nullopt;
    }

    auto [time_base, fractional_ns] = ExtractFractionalSeconds(trimmed_time);
    if (!time_info.has_fractional) {
      fractional_ns = std::chrono::nanoseconds{0};
    }

    std::istringstream time_in{time_base};
    time_in.imbue(std::locale::classic());
    std::chrono::seconds tod;
    time_in >> date::parse(time_info.format.c_str(), tod);
    if (time_in.fail()) {
      return std::nullopt;
    }

    auto tp = date::sys_days{ymd} +
              std::chrono::duration_cast<std::chrono::nanoseconds>(tod) + fractional_ns;
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace PJ::CSV
