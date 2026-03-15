#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace PJ::CSV {

std::optional<double> toDouble(const std::string& str);

std::string Trim(const std::string& str);

bool IsDayFirstFormat(const std::string& str, char separator);

std::optional<double> AutoParseTimestamp(const std::string& str);

std::optional<double> FormatParseTimestamp(const std::string& str, const std::string& format);

enum class ColumnType {
  NUMBER,
  HEX,
  EPOCH_SECONDS,
  EPOCH_MILLIS,
  EPOCH_MICROS,
  EPOCH_NANOS,
  DATETIME,
  DATE_ONLY,
  TIME_ONLY,
  STRING,
  UNDEFINED
};

struct ColumnTypeInfo {
  ColumnType type = ColumnType::UNDEFINED;
  std::string format;
  bool has_fractional = false;
};

ColumnTypeInfo DetectColumnType(const std::string& str);

std::optional<double> ParseWithType(const std::string& str, const ColumnTypeInfo& type_info);

std::optional<double> ParseCombinedDateTime(const std::string& date_str,
                                            const std::string& time_str,
                                            const ColumnTypeInfo& date_info,
                                            const ColumnTypeInfo& time_info);

}  // namespace PJ::CSV
