#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "timestamp_parsing.hpp"

namespace PJ::CSV {

/// Best-guess delimiter plus whether the header was ambiguous. A header is
/// ambiguous when two or more of the "hard" delimiters (comma / semicolon / tab)
/// each appear — the choice cannot be made confidently and needs the user. Space
/// never contributes to ambiguity (headers routinely contain spaces after
/// commas); it is only the delimiter when no hard delimiter is present.
struct DelimiterDetection {
  char delimiter;  ///< detected delimiter (',' when none found)
  bool ambiguous;  ///< true when >=2 hard delimiters are present
};

DelimiterDetection DetectDelimiterEx(const std::string& first_line);

char DetectDelimiter(const std::string& first_line);

void SplitLine(const std::string& line, char separator, std::vector<std::string>& parts);

std::vector<std::string> ParseHeaderLine(const std::string& header_line, char delimiter);

struct CombinedColumnPair {
  int date_column_index;
  int time_column_index;
  std::string virtual_name;
};

struct CsvParseConfig {
  char delimiter = ',';
  int time_column_index = -1;
  std::string custom_time_format;
  int skip_rows = 0;
  int total_lines = 0;

  std::vector<CombinedColumnPair> combined_columns;
  int combined_column_index = -1;
};

struct CsvColumnData {
  std::string name;
  std::vector<std::pair<double, double>> numeric_points;
  std::vector<std::pair<double, std::string>> string_points;
  ColumnTypeInfo detected_type;
};

struct CsvParseWarning {
  enum Type { WRONG_COLUMN_COUNT, INVALID_TIMESTAMP, NON_MONOTONIC_TIME, DUPLICATE_COLUMN_NAMES };
  Type type;
  int line_number;
  std::string detail;
};

struct CsvParseResult {
  bool success = false;
  std::vector<CsvColumnData> columns;
  std::vector<std::string> column_names;
  std::vector<CsvParseWarning> warnings;
  bool time_is_non_monotonic = false;
  int lines_processed = 0;
  int lines_skipped = 0;
  std::set<int> combined_component_indices;
};

std::vector<CombinedColumnPair> DetectCombinedDateTimeColumns(
    const std::vector<std::string>& column_names, const std::vector<ColumnTypeInfo>& column_types);

CsvParseResult ParseCsvData(
    std::istream& input, const CsvParseConfig& config, std::function<bool(int, int)> progress = nullptr);

CsvParseResult ParseCsvData(
    const std::string& csv_content, const CsvParseConfig& config, std::function<bool(int, int)> progress = nullptr);

}  // namespace PJ::CSV
