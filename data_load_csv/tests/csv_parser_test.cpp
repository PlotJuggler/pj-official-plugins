#include "../csv_parser.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

using namespace PJ::CSV;

// --- DetectDelimiter ---

TEST(CsvDelimiter, DetectsComma) {
  EXPECT_EQ(DetectDelimiter("a,b,c"), ',');
}

TEST(CsvDelimiter, DetectsTab) {
  EXPECT_EQ(DetectDelimiter("a\tb\tc"), '\t');
}

TEST(CsvDelimiter, DetectsSemicolon) {
  EXPECT_EQ(DetectDelimiter("a;b;c"), ';');
}

TEST(CsvDelimiter, IgnoresDelimiterInsideQuotes) {
  EXPECT_EQ(DetectDelimiter(R"("a,b",c,d)"), ',');
}

TEST(CsvDelimiter, DefaultsToComma) {
  EXPECT_EQ(DetectDelimiter("single_field"), ',');
}

// --- SplitLine ---

TEST(CsvSplitLine, SimpleComma) {
  std::vector<std::string> parts;
  SplitLine("a,b,c", ',', parts);
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "b");
  EXPECT_EQ(parts[2], "c");
}

TEST(CsvSplitLine, QuotedField) {
  std::vector<std::string> parts;
  SplitLine(R"("hello, world",b,c)", ',', parts);
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "hello, world");
}

TEST(CsvSplitLine, TrailingSeparator) {
  std::vector<std::string> parts;
  SplitLine("a,b,", ',', parts);
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[2], "");
}

// --- ParseHeaderLine ---

TEST(CsvHeader, NormalHeader) {
  auto names = ParseHeaderLine("time,x,y,z", ',');
  ASSERT_EQ(names.size(), 4u);
  EXPECT_EQ(names[0], "time");
  EXPECT_EQ(names[3], "z");
}

TEST(CsvHeader, NumericOnlyHeaderGeneratesNames) {
  auto names = ParseHeaderLine("1,2,3", ',');
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "_Column_0");
  EXPECT_EQ(names[2], "_Column_2");
}

TEST(CsvHeader, DuplicateNamesGetSuffix) {
  auto names = ParseHeaderLine("val,val,other", ',');
  ASSERT_EQ(names.size(), 3u);
  EXPECT_NE(names[0], names[1]);
}

// --- ParseCsvData ---

TEST(CsvParse, SimpleNumeric) {
  std::string csv = "time,x,y\n0,1.0,2.0\n1,3.0,4.0\n2,5.0,6.0\n";
  CsvParseConfig config;
  config.delimiter = ',';
  config.time_column_index = 0;
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 3);
  EXPECT_EQ(result.column_names.size(), 3u);

  // Column "x" (index 1) should have 3 numeric points
  ASSERT_EQ(result.columns[1].numeric_points.size(), 3u);
  EXPECT_DOUBLE_EQ(result.columns[1].numeric_points[0].second, 1.0);
  EXPECT_DOUBLE_EQ(result.columns[1].numeric_points[2].second, 5.0);
}

TEST(CsvParse, AutoRowNumberTimestamp) {
  std::string csv = "x,y\n1.0,2.0\n3.0,4.0\n";
  CsvParseConfig config;
  config.delimiter = ',';
  // time_column_index = -1 means use row number
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 2);
  // Timestamps should be row indices: 0, 1
  EXPECT_DOUBLE_EQ(result.columns[0].numeric_points[0].first, 0.0);
  EXPECT_DOUBLE_EQ(result.columns[0].numeric_points[1].first, 1.0);
}

TEST(CsvParse, WrongColumnCountSkipped) {
  std::string csv = "a,b\n1,2\n3\n4,5\n";
  CsvParseConfig config;
  config.delimiter = ',';
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 2);
  EXPECT_EQ(result.lines_skipped, 1);
  ASSERT_FALSE(result.warnings.empty());
  EXPECT_EQ(result.warnings[0].type, CsvParseWarning::WRONG_COLUMN_COUNT);
}

TEST(CsvParse, EmptyLinesSkipped) {
  std::string csv = "a,b\n1,2\n\n3,4\n";
  CsvParseConfig config;
  config.delimiter = ',';
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 2);
}

TEST(CsvParse, WindowsLineEndings) {
  std::string csv = "a,b\r\n1,2\r\n3,4\r\n";
  CsvParseConfig config;
  config.delimiter = ',';
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 2);
  EXPECT_EQ(result.column_names[0], "a");
  EXPECT_EQ(result.column_names[1], "b");
}

TEST(CsvParse, SkipRows) {
  std::string csv = "# comment\n# comment2\ntime,val\n0,1\n1,2\n";
  CsvParseConfig config;
  config.delimiter = ',';
  config.skip_rows = 2;
  config.time_column_index = 0;
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.column_names[0], "time");
  EXPECT_EQ(result.lines_processed, 2);
}

TEST(CsvParse, TabDelimited) {
  std::string csv = "a\tb\n1\t2\n3\t4\n";
  CsvParseConfig config;
  config.delimiter = '\t';
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 2);
}

TEST(CsvParse, CancellationViaProgress) {
  std::string csv = "a\n";
  for (int i = 0; i < 300; i++) {
    csv += std::to_string(i) + "\n";
  }
  CsvParseConfig config;
  config.delimiter = ',';
  auto result = ParseCsvData(csv, config, [](int, int) -> bool {
    return false;  // cancel immediately
  });

  // Should have processed fewer than all 300 rows
  EXPECT_LT(result.lines_processed, 300);
}

// --- Timestamp parsing ---

TEST(CsvTimestamp, ToDouble) {
  EXPECT_DOUBLE_EQ(*toDouble("3.14"), 3.14);
  EXPECT_DOUBLE_EQ(*toDouble("42"), 42.0);
  EXPECT_DOUBLE_EQ(*toDouble("1e3"), 1000.0);
  EXPECT_FALSE(toDouble("abc").has_value());
}

TEST(CsvTimestamp, EuropeanDecimal) {
  // Single comma treated as decimal separator
  EXPECT_DOUBLE_EQ(*toDouble("3,14"), 3.14);
}

TEST(CsvTimestamp, DetectColumnTypeNumber) {
  auto info = DetectColumnType("3.14");
  EXPECT_EQ(info.type, ColumnType::NUMBER);
}

TEST(CsvTimestamp, DetectColumnTypeHex) {
  auto info = DetectColumnType("0xFF");
  EXPECT_EQ(info.type, ColumnType::HEX);
}

TEST(CsvTimestamp, DetectColumnTypeString) {
  auto info = DetectColumnType("hello");
  EXPECT_EQ(info.type, ColumnType::STRING);
}

TEST(CsvTimestamp, DetectColumnTypeDatetime) {
  auto info = DetectColumnType("2024-01-15 14:30:25");
  EXPECT_EQ(info.type, ColumnType::DATETIME);
}

TEST(CsvTimestamp, DetectColumnTypeDateOnly) {
  auto info = DetectColumnType("2024-01-15");
  EXPECT_EQ(info.type, ColumnType::DATE_ONLY);
}

TEST(CsvTimestamp, DetectColumnTypeTimeOnly) {
  auto info = DetectColumnType("14:30:25");
  EXPECT_EQ(info.type, ColumnType::TIME_ONLY);
}

TEST(CsvTimestamp, DetectEpochSeconds) {
  auto info = DetectColumnType("1700000000");
  EXPECT_EQ(info.type, ColumnType::EPOCH_SECONDS);
}

TEST(CsvTimestamp, DetectEpochMillis) {
  auto info = DetectColumnType("1700000000000");
  EXPECT_EQ(info.type, ColumnType::EPOCH_MILLIS);
}

TEST(CsvTimestamp, ParseWithTypeNumber) {
  ColumnTypeInfo info;
  info.type = ColumnType::NUMBER;
  EXPECT_DOUBLE_EQ(*ParseWithType("42.5", info), 42.5);
}

TEST(CsvTimestamp, ParseWithTypeEpoch) {
  ColumnTypeInfo info;
  info.type = ColumnType::EPOCH_SECONDS;
  auto val = ParseWithType("1700000000", info);
  ASSERT_TRUE(val.has_value());
  EXPECT_DOUBLE_EQ(*val, 1700000000.0);
}

TEST(CsvTimestamp, ParseIso8601) {
  auto ts = AutoParseTimestamp("2024-01-15T14:30:25");
  ASSERT_TRUE(ts.has_value());
  EXPECT_GT(*ts, 0.0);
}

TEST(CsvTimestamp, ParseFractionalSeconds) {
  auto ts = AutoParseTimestamp("2024-01-15T14:30:25.123456789");
  ASSERT_TRUE(ts.has_value());
  EXPECT_GT(*ts, 0.0);
}

TEST(CsvTimestamp, Trim) {
  EXPECT_EQ(Trim("  hello  "), "hello");
  EXPECT_EQ(Trim("\t\n"), "");
  EXPECT_EQ(Trim("no_space"), "no_space");
}

}  // namespace
