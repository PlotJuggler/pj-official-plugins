#include "export_core.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <gtest/gtest.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>

#include <bit>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string tempPath(const std::string& filename) {
  return ::testing::TempDir() + filename;
}

std::string readFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(Utf8Path, RoundTripsProtocolBytesThroughFilesystemPath) {
  constexpr std::string_view utf8 = "übung_日本語.csv";
  EXPECT_EQ(pathToUtf8(pathFromUtf8(utf8)), utf8);
}

TEST(BuildExportTable, AlignedSeriesMergeWithoutDrift) {
  const std::vector<NumericSeriesData> numeric{
      {"first", {0.0, 1.0, 2.0}, {10.0, 11.0, 12.0}},
      {"second", {0.0, 1.0, 2.0}, {20.0, 21.0, 22.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 2.0);

  EXPECT_EQ(table.time, (std::vector<double>{0.0, 1.0, 2.0}));
  EXPECT_EQ(table.names, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(table.cols[0], numeric[0].v);
  EXPECT_EQ(table.cols[1], numeric[1].v);
  EXPECT_EQ(table.has_value[0], (std::vector<uint8_t>{1, 1, 1}));
  EXPECT_EQ(table.has_value[1], (std::vector<uint8_t>{1, 1, 1}));
}

TEST(BuildExportTable, InclusiveHalfPeriodToleranceMergesBoundary) {
  const std::vector<NumericSeriesData> numeric{
      {"anchor", {0.0, 2.0}, {10.0, 20.0}},
      {"half_period_offset", {1.0, 3.0}, {11.0, 21.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 3.0);

  EXPECT_EQ(table.time, (std::vector<double>{0.0, 2.0}));
  EXPECT_EQ(table.cols[0], (std::vector<double>{10.0, 20.0}));
  EXPECT_EQ(table.cols[1], (std::vector<double>{11.0, 21.0}));
  EXPECT_EQ(table.has_value[0], (std::vector<uint8_t>{1, 1}));
  EXPECT_EQ(table.has_value[1], (std::vector<uint8_t>{1, 1}));
}

TEST(BuildExportTable, MinimumTimestampLetsSeveralSeriesAdvance) {
  const std::vector<NumericSeriesData> numeric{
      {"anchor", {0.0, 4.0}, {10.0, 14.0}},
      {"middle", {0.5, 4.5}, {20.0, 24.0}},
      {"edge", {2.0, 6.0}, {30.0, 34.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 6.0);

  ASSERT_FALSE(table.time.empty());
  EXPECT_DOUBLE_EQ(table.time[0], 0.0);
  EXPECT_EQ(table.has_value[0][0], 1);
  EXPECT_EQ(table.has_value[1][0], 1);
  EXPECT_EQ(table.has_value[2][0], 1);
  EXPECT_DOUBLE_EQ(table.cols[0][0], 10.0);
  EXPECT_DOUBLE_EQ(table.cols[1][0], 20.0);
  EXPECT_DOUBLE_EQ(table.cols[2][0], 30.0);
}

TEST(BuildExportTable, UnmatchedCurrentPointRemainsPending) {
  const std::vector<NumericSeriesData> numeric{
      {"anchor", {0.0, 2.0, 4.0}, {10.0, 20.0, 40.0}},
      {"pending", {1.25, 3.25}, {125.0, 325.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 4.0);

  ASSERT_EQ(table.time, (std::vector<double>{0.0, 1.25, 3.25}));
  EXPECT_EQ(table.has_value[1], (std::vector<uint8_t>{0, 1, 1}));
  EXPECT_TRUE(std::isnan(table.cols[1][0]));
  EXPECT_DOUBLE_EQ(table.cols[1][1], 125.0);
  EXPECT_DOUBLE_EQ(table.cols[1][2], 325.0);
}

TEST(BuildExportTable, NumericAndStringSeriesKeepTypeOrderingAndInterleave) {
  const std::vector<NumericSeriesData> numeric{
      {"num_a", {0.0, 2.0}, {10.0, 20.0}},
      {"num_b", {1.25, 3.25}, {11.0, 21.0}},
  };
  const std::vector<StringSeriesData> strings{
      {"str_a", {0.0, 2.0}, {"a0", "a2"}},
      {"str_b", {1.25, 3.25}, {"b1", "b3"}},
  };

  const ExportTable table = buildExportTable(numeric, strings, 0.0, 3.25);

  EXPECT_EQ(table.names, (std::vector<std::string>{"num_a", "num_b"}));
  EXPECT_EQ(table.string_names, (std::vector<std::string>{"str_a", "str_b"}));
  EXPECT_EQ(table.time, (std::vector<double>{0.0, 1.25, 3.25}));
  EXPECT_EQ(table.has_value[1], (std::vector<uint8_t>{0, 1, 1}));
  EXPECT_EQ(table.string_has_value[0], (std::vector<uint8_t>{1, 1, 0}));
  EXPECT_EQ(table.string_has_value[1], (std::vector<uint8_t>{0, 1, 1}));
  EXPECT_EQ(table.string_cols[0], (std::vector<std::string>{"a0", "a2", ""}));
  EXPECT_EQ(table.string_cols[1], (std::vector<std::string>{"", "b1", "b3"}));
}

TEST(BuildExportTable, ZeroToleranceRequiresExactEquality) {
  const double near = std::nextafter(1.0, 2.0);
  const std::vector<NumericSeriesData> numeric{
      {"exact_a", {1.0}, {10.0}},
      {"exact_b", {1.0}, {20.0}},
      {"near", {near}, {30.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 2.0);

  ASSERT_EQ(table.time.size(), 2U);
  EXPECT_DOUBLE_EQ(table.time[0], 1.0);
  EXPECT_DOUBLE_EQ(table.time[1], near);
  EXPECT_EQ(table.has_value[0], (std::vector<uint8_t>{1, 0}));
  EXPECT_EQ(table.has_value[1], (std::vector<uint8_t>{1, 0}));
  EXPECT_EQ(table.has_value[2], (std::vector<uint8_t>{0, 1}));
}

TEST(BuildExportTable, DuplicateTimestampsAdvanceOneSamplePerRow) {
  const std::vector<NumericSeriesData> numeric{
      {"duplicates", {0.0, 0.0, 1.0}, {10.0, 11.0, 12.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 0.0, 1.0);

  EXPECT_EQ(table.time, (std::vector<double>{0.0, 0.0, 1.0}));
  EXPECT_EQ(table.cols[0], (std::vector<double>{10.0, 11.0, 12.0}));
  EXPECT_EQ(table.has_value[0], (std::vector<uint8_t>{1, 1, 1}));
}

TEST(EstimateMinDt, ScanWindowStopsAfterFirstTwoThousandIntervals) {
  std::vector<double> timestamps;
  timestamps.reserve(2002);
  for (size_t idx = 0; idx <= 2000; ++idx) {
    timestamps.push_back(static_cast<double>(idx));
  }
  timestamps.push_back(2000.25);

  EXPECT_DOUBLE_EQ(estimateMinDt(timestamps, 0, 3000.0), 1.0);
}

TEST(EstimateMinDt, IntervalStraddlingEndIsStillCounted) {
  EXPECT_DOUBLE_EQ(estimateMinDt({0.0, 2.0, 2.5}, 0, 1.0), 2.0);
}

TEST(EstimateMinDt, AllNonpositiveIntervalsReturnZero) {
  EXPECT_DOUBLE_EQ(estimateMinDt({3.0, 2.0, 2.0, 1.0}, 0, 10.0), 0.0);
}

TEST(IndexFromTime, EmptyVectorReturnsMinusOne) {
  EXPECT_EQ(indexFromTime({}, 1.0), -1);
}

TEST(IndexFromTime, StrictlyCloserPredecessorIsChosenAndExportedBeforeStart) {
  EXPECT_EQ(indexFromTime({0.0, 10.0}, 3.0), 0);

  const std::vector<NumericSeriesData> numeric{{"sample", {0.0, 10.0}, {1.0, 2.0}}};
  const ExportTable table = buildExportTable(numeric, {}, 3.0, 10.0);
  ASSERT_EQ(table.time.size(), 2U);
  EXPECT_DOUBLE_EQ(table.time[0], 0.0);
  EXPECT_DOUBLE_EQ(table.cols[0][0], 1.0);
}

TEST(IndexFromTime, ExactMidpointChoosesLaterIndex) {
  EXPECT_EQ(indexFromTime({0.0, 10.0}, 5.0), 1);
}

TEST(IndexFromTime, ExactExistingTimestampIsChosen) {
  EXPECT_EQ(indexFromTime({0.0, 10.0, 20.0}, 10.0), 1);
}

TEST(IndexFromTime, BeforeFirstChoosesFirstIndex) {
  EXPECT_EQ(indexFromTime({10.0, 20.0}, 5.0), 0);
}

TEST(IndexFromTime, AfterLastChoosesLastIndex) {
  EXPECT_EQ(indexFromTime({10.0, 20.0}, 25.0), 1);
}

TEST(BuildExportTable, OutOfRangeAndEmptySeriesAreExcluded) {
  const std::vector<NumericSeriesData> numeric{
      {"empty", {}, {}},
      {"before", {0.0, 1.0}, {10.0, 11.0}},
      {"after", {10.0, 11.0}, {20.0, 21.0}},
      {"inside", {4.0, 6.0}, {30.0, 31.0}},
  };

  const ExportTable table = buildExportTable(numeric, {}, 3.0, 7.0);

  EXPECT_EQ(table.names, (std::vector<std::string>{"inside"}));
}

TEST(SerializeCsv, QtVerifiedNumericGolden) {
  ExportTable table;
  table.names = {"number"};
  table.time = {-0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0};
  table.cols = {
      {0.0, -0.0, 1.5, 1234567.891234, 1e13, 1.25e-5, 5e-320, 0.1 + 0.2, std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 42.0}};
  table.has_value = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}};

  const std::string path = tempPath("data_exporter_numeric_golden.csv");
  ASSERT_TRUE(serializeCSV(table, path));

  const std::string expected =
      "time,number\n"
      "0.000000,0\n"
      "1.000000,0\n"
      "2.000000,1.5\n"
      "3.000000,1234567.89123\n"
      "4.000000,1e+13\n"
      "5.000000,1.25e-05\n"
      "6.000000,4.99994433591e-320\n"
      "7.000000,0.3\n"
      "8.000000,NaN\n"
      "9.000000,Inf\n"
      "10.000000,Inf\n"
      "11.000000,\n";
  EXPECT_EQ(readFile(path), expected);
}

TEST(SerializeCsv, QtCompatibleStringQuotingGolden) {
  ExportTable table;
  table.time = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  table.string_names = {"text"};
  table.string_cols = {{"plain", "with,comma", "with\"quote", "line\nfeed", "carriage\rreturn", "", "ignored"}};
  table.string_has_value = {{1, 1, 1, 1, 1, 1, 0}};

  const std::string path = tempPath("data_exporter_string_golden.csv");
  ASSERT_TRUE(serializeCSV(table, path));

  const std::string expected =
      "time,text\n"
      "0.000000,plain\n"
      "1.000000,\"with,comma\"\n"
      "2.000000,\"with\"\"quote\"\n"
      "3.000000,\"line\nfeed\"\n"
      "4.000000,\"carriage\rreturn\"\n"
      "5.000000,\n"
      "6.000000,\n";
  EXPECT_EQ(readFile(path), expected);
}

TEST(SerializeCsv, HeaderNamesContainingCommasRemainUnquoted) {
  ExportTable table;
  table.names = {"numeric,name"};
  table.time = {0.0};
  table.cols = {{1.0}};
  table.has_value = {{1}};
  table.string_names = {"string,name"};
  table.string_cols = {{"value"}};
  table.string_has_value = {{1}};

  const std::string path = tempPath("data_exporter_unquoted_header.csv");
  ASSERT_TRUE(serializeCSV(table, path));
  EXPECT_EQ(readFile(path), "time,numeric,name,string,name\n0.000000,1,value\n");
}

TEST(SerializeCsv, NumericFormattingIsIndependentOfProcessLocale) {
  const char* current_locale = std::setlocale(LC_ALL, nullptr);
  ASSERT_NE(current_locale, nullptr);
  const std::string original_locale(current_locale);

  // Any installed comma-decimal locale proves the point; de_DE is the classic
  // choice but en_DK ships even on English-only systems.
  const char* comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "en_DK.UTF-8", "en_DK.utf8", "fr_FR.UTF-8"};
  bool locale_set = false;
  for (const char* candidate : comma_locales) {
    if (std::setlocale(LC_ALL, candidate) != nullptr) {
      locale_set = true;
      break;
    }
  }
  if (!locale_set) {
    GTEST_SKIP() << "no comma-decimal locale is installed";
  }
  ASSERT_EQ(std::string(localeconv()->decimal_point), ",") << "picked locale does not use comma decimals";

  ExportTable table;
  table.names = {"number"};
  table.time = {1.5};
  table.cols = {{1234.5}};
  table.has_value = {{1}};
  const std::string path = tempPath("data_exporter_locale.csv");
  const bool serialized = serializeCSV(table, path);
  const std::string contents = readFile(path);
  (void)std::setlocale(LC_ALL, original_locale.c_str());

  ASSERT_TRUE(serialized);
  EXPECT_EQ(contents, "time,number\n1.500000,1234.5\n");
}

TEST(Serializers, Pj3TablePreconditionsAreShared) {
  const std::string csv_path = tempPath("data_exporter_invalid.csv");
  const std::string parquet_path = tempPath("data_exporter_invalid.parquet");
  EXPECT_FALSE(serializeCSV({}, csv_path));
  EXPECT_FALSE(serializeParquet({}, parquet_path));

  ExportTable mismatch;
  mismatch.time = {0.0};
  mismatch.names = {"missing_column"};
  EXPECT_FALSE(serializeCSV(mismatch, csv_path));
  EXPECT_FALSE(serializeParquet(mismatch, parquet_path));
}

TEST(SerializeParquet, RoundTripsSchemaNullsSpecialValuesAndDefaultCompression) {
  ExportTable source;
  source.names = {"signal"};
  source.time = {0.0, 1.0, 2.0, 3.0, 4.0};
  source.cols = {
      {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
       -std::numeric_limits<double>::infinity(), 42.0, 7.0}};
  source.has_value = {{1, 1, 1, 0, 1}};
  source.string_names = {"text"};
  source.string_cols = {{"", "alpha", "ignored", "beta", "ignored-too"}};
  source.string_has_value = {{1, 1, 0, 1, 0}};

  const std::string path = tempPath("data_exporter_round_trip.parquet");
  ASSERT_TRUE(serializeParquet(source, path));

  auto input_result = arrow::io::ReadableFile::Open(path);
  ASSERT_TRUE(input_result.ok()) << input_result.status().ToString();
  auto reader_result = parquet::arrow::OpenFile(*input_result, arrow::default_memory_pool());
  ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
  std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_result);

  std::shared_ptr<arrow::Table> result;
  const arrow::Status read_status = reader->ReadTable(&result);
  ASSERT_TRUE(read_status.ok()) << read_status.ToString();
  ASSERT_NE(result, nullptr);

  ASSERT_EQ(result->num_columns(), 3);
  EXPECT_EQ(result->schema()->field(0)->name(), "time");
  EXPECT_EQ(result->schema()->field(1)->name(), "signal");
  EXPECT_EQ(result->schema()->field(2)->name(), "text");
  EXPECT_EQ(result->schema()->field(0)->type()->id(), arrow::Type::DOUBLE);
  EXPECT_EQ(result->schema()->field(1)->type()->id(), arrow::Type::DOUBLE);
  EXPECT_EQ(result->schema()->field(2)->type()->id(), arrow::Type::STRING);
  EXPECT_EQ(result->column(0)->null_count(), 0);
  EXPECT_EQ(result->column(1)->null_count(), 1);
  EXPECT_EQ(result->column(2)->null_count(), 2);

  ASSERT_EQ(result->column(1)->num_chunks(), 1);
  const auto numeric = std::static_pointer_cast<arrow::DoubleArray>(result->column(1)->chunk(0));
  EXPECT_TRUE(numeric->IsValid(0));
  EXPECT_EQ(std::bit_cast<uint64_t>(numeric->Value(0)), std::bit_cast<uint64_t>(source.cols[0][0]));
  EXPECT_TRUE(numeric->IsValid(1));
  EXPECT_EQ(std::bit_cast<uint64_t>(numeric->Value(1)), std::bit_cast<uint64_t>(source.cols[0][1]));
  EXPECT_TRUE(numeric->IsValid(2));
  EXPECT_EQ(std::bit_cast<uint64_t>(numeric->Value(2)), std::bit_cast<uint64_t>(source.cols[0][2]));
  EXPECT_TRUE(numeric->IsNull(3));
  EXPECT_TRUE(numeric->IsValid(4));
  EXPECT_DOUBLE_EQ(numeric->Value(4), 7.0);

  ASSERT_EQ(result->column(2)->num_chunks(), 1);
  const auto text = std::static_pointer_cast<arrow::StringArray>(result->column(2)->chunk(0));
  EXPECT_TRUE(text->IsValid(0));
  EXPECT_EQ(text->GetString(0), "");
  EXPECT_TRUE(text->IsValid(1));
  EXPECT_EQ(text->GetString(1), "alpha");
  EXPECT_TRUE(text->IsNull(2));
  EXPECT_TRUE(text->IsValid(3));
  EXPECT_EQ(text->GetString(3), "beta");
  EXPECT_TRUE(text->IsNull(4));

  const std::shared_ptr<parquet::FileMetaData> metadata = reader->parquet_reader()->metadata();
  ASSERT_EQ(metadata->num_row_groups(), 1);
  const parquet::Compression::type actual_codec = metadata->RowGroup(0)->ColumnChunk(0)->compression();
  const auto default_properties = parquet::WriterProperties::Builder{}.build();
  const auto time_path = parquet::schema::ColumnPath::FromDotString("time");
  EXPECT_EQ(actual_codec, default_properties->compression(time_path));
  EXPECT_EQ(actual_codec, parquet::Compression::UNCOMPRESSED);
  for (int column = 0; column < metadata->num_columns(); ++column) {
    EXPECT_EQ(metadata->RowGroup(0)->ColumnChunk(column)->compression(), actual_codec);
  }
}

TEST(SerializeParquet, MoreThanChunkSizeRowsProduceMultipleRowGroups) {
  constexpr size_t row_count = (1024U * 64U) + 11U;
  ExportTable source;
  source.names = {"signal"};
  source.time.reserve(row_count);
  source.cols.resize(1);
  source.cols[0].reserve(row_count);
  source.has_value.resize(1);
  source.has_value[0].assign(row_count, 1);
  for (size_t row = 0; row < row_count; ++row) {
    source.time.push_back(static_cast<double>(row));
    source.cols[0].push_back(static_cast<double>(row) * 2.0);
  }

  const std::string path = tempPath("data_exporter_multiple_row_groups.parquet");
  ASSERT_TRUE(serializeParquet(source, path));

  auto input_result = arrow::io::ReadableFile::Open(path);
  ASSERT_TRUE(input_result.ok()) << input_result.status().ToString();
  auto reader_result = parquet::arrow::OpenFile(*input_result, arrow::default_memory_pool());
  ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
  std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_result);
  std::shared_ptr<arrow::Table> result;
  const arrow::Status read_status = reader->ReadTable(&result);
  ASSERT_TRUE(read_status.ok()) << read_status.ToString();

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->num_rows(), static_cast<int64_t>(row_count));
  EXPECT_GT(reader->parquet_reader()->metadata()->num_row_groups(), 1);
}

}  // namespace
