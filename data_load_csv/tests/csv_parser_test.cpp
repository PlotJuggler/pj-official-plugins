#include "../csv_parser.hpp"

#include <gtest/gtest.h>

#include <clocale>
#include <cstdlib>
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

// --- DetectDelimiterEx ambiguity ---

TEST(CsvDelimiterEx, SingleHardDelimiterIsUnambiguous) {
  EXPECT_FALSE(DetectDelimiterEx("a,b,c").ambiguous);
  EXPECT_FALSE(DetectDelimiterEx("a;b;c").ambiguous);
  EXPECT_FALSE(DetectDelimiterEx("a\tb\tc").ambiguous);
}

TEST(CsvDelimiterEx, SpaceDoesNotCauseAmbiguity) {
  // A comma header with spaces after the commas stays unambiguous (space is only
  // the delimiter when no hard delimiter is present).
  const auto det = DetectDelimiterEx("time, value, x");
  EXPECT_FALSE(det.ambiguous);
  EXPECT_EQ(det.delimiter, ',');
}

TEST(CsvDelimiterEx, TwoHardDelimitersAreAmbiguous) {
  EXPECT_TRUE(DetectDelimiterEx("a,b;c").ambiguous);
  EXPECT_TRUE(DetectDelimiterEx("a\tb,c").ambiguous);
}

TEST(CsvDelimiterEx, PureSpaceIsUnambiguous) {
  const auto det = DetectDelimiterEx("a b c");
  EXPECT_FALSE(det.ambiguous);
  EXPECT_EQ(det.delimiter, ' ');
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

// An RFC-4180 escaped double-quote ("") inside a quoted field must become a
// single literal quote in the output, not truncate or empty the field.
TEST(CsvSplitLine, EscapedQuoteInsideQuotedField) {
  std::vector<std::string> parts;
  SplitLine(R"(a,"he said ""hi""",b)", ',', parts);
  ASSERT_EQ(parts.size(), 3u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], R"(he said "hi")");
  EXPECT_EQ(parts[2], "b");
}

TEST(CsvSplitLine, EscapedQuoteAtFieldStart) {
  std::vector<std::string> parts;
  SplitLine(R"("a""b",c)", ',', parts);
  ASSERT_EQ(parts.size(), 2u);
  EXPECT_EQ(parts[0], R"(a"b)");
  EXPECT_EQ(parts[1], "c");
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

// End-to-end mirror of test_data/test_escaped_quotes.csv. The reported symptom
// was that a header like "temp ""degC""" ended up nameless (_Column_N) because
// SplitLine emitted an empty string; assert the escape-collapsed header AND
// that the numeric column downstream still lands on the right index.
TEST(CsvParse, EscapedQuotesHeaderAndData) {
  std::string csv =
      "time,\"temp \"\"degC\"\"\",value\n"
      "0,10,100\n"
      "1,11,101\n"
      "2,12,102\n"
      "3,13,103\n";
  CsvParseConfig config;
  config.delimiter = ',';
  config.time_column_index = 0;
  auto result = ParseCsvData(csv, config);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.lines_processed, 4);
  ASSERT_EQ(result.column_names.size(), 3u);
  EXPECT_EQ(result.column_names[0], "time");
  EXPECT_EQ(result.column_names[1], R"(temp "degC")");
  EXPECT_EQ(result.column_names[2], "value");
  ASSERT_EQ(result.columns[1].numeric_points.size(), 4u);
  EXPECT_DOUBLE_EQ(result.columns[1].numeric_points[0].second, 10.0);
  EXPECT_DOUBLE_EQ(result.columns[1].numeric_points[3].second, 13.0);
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

// --- Locale independence (issue #238) ---
//
// The plugin runs inside a Qt host, and QCoreApplication calls
// setlocale(LC_ALL, "") at startup. On comma-decimal systems (fr_FR, es_ES,
// de_DE, ...) that makes std::strtod treat ',' as the decimal separator and
// reject "1781610745.99047" — every row of a dot-decimal CSV was skipped as
// "Invalid timestamp". Parsing must not depend on the process locale.

constexpr const char* kIssue238Csv =
    "time_epoch,data1,data2\n"
    "1781610745.99047,5.0414,-1.0\n"
    "1781610746.51634,5.0414,-2.0\n"
    "1781610747.85435,5.0510,-3.0\n";

void expectIssue238CsvLoads() {
  CsvParseConfig config;
  config.time_column_index = 0;
  auto result = ParseCsvData(kIssue238Csv, config);
  EXPECT_EQ(result.lines_processed, 3);
  EXPECT_EQ(result.lines_skipped, 0);
  EXPECT_TRUE(result.warnings.empty());
  ASSERT_EQ(result.columns.size(), 3u);
  ASSERT_EQ(result.columns[0].numeric_points.size(), 3u);
  EXPECT_DOUBLE_EQ(result.columns[0].numeric_points.front().first, 1781610745.99047);
}

TEST(CsvLocale, Issue238CsvLoadsInDefaultLocale) {
  expectIssue238CsvLoads();
}

// Finds a comma-decimal locale usable with setlocale(LC_NUMERIC, ...). On
// POSIX, if none is installed, compiles one into a scratch directory with
// localedef and points LOCPATH at it — CI images usually ship only C/English
// locales, and a skipped test would leave this regression uncovered.
std::string commaDecimalLocaleName() {
#if defined(_WIN32)
  const char* candidates[] = {"French_France.1252", "fr-FR", "Spanish_Spain.1252", "es-ES"};
#else
  const char* candidates[] = {"fr_FR.UTF-8", "es_ES.UTF-8", "de_DE.UTF-8", "fr_FR", "es_ES"};
#endif
  for (const char* name : candidates) {
    if (std::setlocale(LC_NUMERIC, name) != nullptr) {
      std::setlocale(LC_NUMERIC, "C");
      return name;
    }
  }
#if !defined(_WIN32)
  const std::string locale_dir = ::testing::TempDir() + "csv_locale_test";
  const std::string command = "localedef -i fr_FR -f UTF-8 '" + locale_dir + "/fr_FR.UTF-8' 2>/dev/null";
  if (std::system(("mkdir -p '" + locale_dir + "'").c_str()) == 0 && std::system(command.c_str()) == 0 &&
      setenv("LOCPATH", locale_dir.c_str(), 1) == 0 && std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr) {
    std::setlocale(LC_NUMERIC, "C");
    return "fr_FR.UTF-8";
  }
#endif
  return {};
}

class CsvCommaDecimalLocale : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string name = commaDecimalLocaleName();
    if (name.empty()) {
      GTEST_SKIP() << "no comma-decimal locale available and localedef generation failed";
    }
    ASSERT_NE(std::setlocale(LC_NUMERIC, name.c_str()), nullptr);
    // Sanity: the locale must actually flip the decimal separator, otherwise
    // these tests silently degrade into the default-locale ones.
    ASSERT_STREQ(std::localeconv()->decimal_point, ",");
  }

  void TearDown() override {
    std::setlocale(LC_NUMERIC, "C");
  }
};

TEST_F(CsvCommaDecimalLocale, ToDoubleParsesDotDecimal) {
  auto value = toDouble("1781610745.99047");
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(*value, 1781610745.99047);
}

TEST_F(CsvCommaDecimalLocale, ToDoubleStillNormalizesEuropeanComma) {
  auto value = toDouble("5,0414");
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(*value, 5.0414);
}

TEST_F(CsvCommaDecimalLocale, ToDoubleRejectsGarbage) {
  EXPECT_FALSE(toDouble("12.34abc").has_value());
  EXPECT_FALSE(toDouble("abc").has_value());
  EXPECT_FALSE(toDouble("").has_value());
}

TEST_F(CsvCommaDecimalLocale, AutoParseTimestampDecimalEpoch) {
  auto ts = AutoParseTimestamp("1781610745.99047");
  ASSERT_TRUE(ts.has_value());
  EXPECT_DOUBLE_EQ(*ts, 1781610745.99047);
}

TEST_F(CsvCommaDecimalLocale, Issue238CsvLoadsCompletely) {
  expectIssue238CsvLoads();
}

}  // namespace
