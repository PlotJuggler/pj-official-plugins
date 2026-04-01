#include "../parquet_helpers.hpp"

#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/builder.h>

namespace {

using namespace PJ::ParquetHelpers;

// --- basenameWithoutExt ---

TEST(ParquetHelpersTest, BasenameWithoutExtUnix) {
  EXPECT_EQ(basenameWithoutExt("/home/user/data/file.parquet"), "file");
}

TEST(ParquetHelpersTest, BasenameWithoutExtWindows) {
  EXPECT_EQ(basenameWithoutExt("C:\\Users\\data\\file.parquet"), "file");
}

TEST(ParquetHelpersTest, BasenameWithoutExtMultipleDots) {
  EXPECT_EQ(basenameWithoutExt("/path/to/data.backup.parquet"), "data.backup");
}

TEST(ParquetHelpersTest, BasenameWithoutExtNoExtension) {
  EXPECT_EQ(basenameWithoutExt("/path/to/filename"), "filename");
}

TEST(ParquetHelpersTest, BasenameWithoutExtJustFilename) {
  EXPECT_EQ(basenameWithoutExt("myfile.txt"), "myfile");
}

TEST(ParquetHelpersTest, BasenameWithoutExtEmptyString) {
  EXPECT_EQ(basenameWithoutExt(""), "");
}

TEST(ParquetHelpersTest, BasenameWithoutExtHiddenFile) {
  EXPECT_EQ(basenameWithoutExt("/path/.hidden"), ".hidden");
}

// --- isSupportedArrowType ---

TEST(ParquetHelpersTest, IsSupportedArrowTypeBool) {
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::BOOL));
}

TEST(ParquetHelpersTest, IsSupportedArrowTypeIntegers) {
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::INT8));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::INT16));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::INT32));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::INT64));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::UINT8));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::UINT16));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::UINT32));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::UINT64));
}

TEST(ParquetHelpersTest, IsSupportedArrowTypeFloats) {
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::FLOAT));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::DOUBLE));
}

TEST(ParquetHelpersTest, IsSupportedArrowTypeStrings) {
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::STRING));
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::LARGE_STRING));
}

TEST(ParquetHelpersTest, IsSupportedArrowTypeTimestamp) {
  EXPECT_TRUE(isSupportedArrowType(arrow::Type::TIMESTAMP));
}

TEST(ParquetHelpersTest, IsSupportedArrowTypeUnsupported) {
  EXPECT_FALSE(isSupportedArrowType(arrow::Type::BINARY));
  EXPECT_FALSE(isSupportedArrowType(arrow::Type::LIST));
  EXPECT_FALSE(isSupportedArrowType(arrow::Type::STRUCT));
  EXPECT_FALSE(isSupportedArrowType(arrow::Type::MAP));
}

// --- arrowTypeToPrimitive ---

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveBool) {
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::BOOL), PJ::PrimitiveType::kBool);
}

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveIntegers) {
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::INT8), PJ::PrimitiveType::kInt8);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::INT16), PJ::PrimitiveType::kInt16);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::INT32), PJ::PrimitiveType::kInt32);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::INT64), PJ::PrimitiveType::kInt64);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::UINT8), PJ::PrimitiveType::kUint8);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::UINT16), PJ::PrimitiveType::kUint16);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::UINT32), PJ::PrimitiveType::kUint32);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::UINT64), PJ::PrimitiveType::kUint64);
}

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveFloats) {
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::FLOAT), PJ::PrimitiveType::kFloat32);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::DOUBLE), PJ::PrimitiveType::kFloat64);
}

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveTimestamp) {
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::TIMESTAMP), PJ::PrimitiveType::kInt64);
}

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveStrings) {
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::STRING), PJ::PrimitiveType::kString);
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::LARGE_STRING), PJ::PrimitiveType::kString);
}

TEST(ParquetHelpersTest, ArrowTypeToPrimitiveDefault) {
  // Unsupported types default to kFloat64
  EXPECT_EQ(arrowTypeToPrimitive(arrow::Type::BINARY), PJ::PrimitiveType::kFloat64);
}

// --- findTimestampColumn ---

TEST(ParquetHelpersTest, FindTimestampColumnByType) {
  auto schema = arrow::schema({
      arrow::field("value", arrow::float64()),
      arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO)),
      arrow::field("name", arrow::utf8()),
  });
  EXPECT_EQ(findTimestampColumn(schema), 1);
}

TEST(ParquetHelpersTest, FindTimestampColumnByName) {
  auto schema = arrow::schema({
      arrow::field("value", arrow::float64()),
      arrow::field("timestamp", arrow::int64()),  // Name match, not type
      arrow::field("name", arrow::utf8()),
  });
  EXPECT_EQ(findTimestampColumn(schema), 1);
}

TEST(ParquetHelpersTest, FindTimestampColumnByNameCaseInsensitive) {
  auto schema = arrow::schema({
      arrow::field("value", arrow::float64()),
      arrow::field("TIMESTAMP", arrow::int64()),
      arrow::field("name", arrow::utf8()),
  });
  EXPECT_EQ(findTimestampColumn(schema), 1);
}

TEST(ParquetHelpersTest, FindTimestampColumnVariousNames) {
  // Test various timestamp name patterns
  std::vector<std::string> names = {"time", "t", "ts", "time_stamp", "datetime",
                                     "date_time", "_timestamp", "_time"};
  for (const auto& name : names) {
    auto schema = arrow::schema({
        arrow::field("value", arrow::float64()),
        arrow::field(name, arrow::int64()),
    });
    EXPECT_EQ(findTimestampColumn(schema), 1) << "Failed for name: " << name;
  }
}

TEST(ParquetHelpersTest, FindTimestampColumnPrefersType) {
  // Type should be preferred over name
  auto schema = arrow::schema({
      arrow::field("timestamp", arrow::int64()),  // Name match
      arrow::field("value", arrow::timestamp(arrow::TimeUnit::NANO)),  // Type match
  });
  EXPECT_EQ(findTimestampColumn(schema), 1);  // Should pick column 1 (type match)
}

TEST(ParquetHelpersTest, FindTimestampColumnNotFound) {
  auto schema = arrow::schema({
      arrow::field("value", arrow::float64()),
      arrow::field("name", arrow::utf8()),
  });
  EXPECT_EQ(findTimestampColumn(schema), -1);
}

// --- getTimestampNanos ---

TEST(ParquetHelpersTest, GetTimestampNanosFromNano) {
  arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::NANO),
                                   arrow::default_memory_pool());
  ASSERT_TRUE(builder.Append(1234567890123456789LL).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::TIMESTAMP), 1234567890123456789LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosFromMicro) {
  arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO),
                                   arrow::default_memory_pool());
  ASSERT_TRUE(builder.Append(1234567890123LL).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::TIMESTAMP), 1234567890123000LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosFromMilli) {
  arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MILLI),
                                   arrow::default_memory_pool());
  ASSERT_TRUE(builder.Append(1234567890LL).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::TIMESTAMP), 1234567890000000LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosFromSecond) {
  arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::SECOND),
                                   arrow::default_memory_pool());
  ASSERT_TRUE(builder.Append(1234567890LL).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::TIMESTAMP), 1234567890000000000LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosFromInt64) {
  arrow::Int64Builder builder;
  ASSERT_TRUE(builder.Append(9876543210LL).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::INT64), 9876543210LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosFromDouble) {
  arrow::DoubleBuilder builder;
  ASSERT_TRUE(builder.Append(1.5).ok());  // 1.5 seconds
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::DOUBLE), 1500000000LL);
}

TEST(ParquetHelpersTest, GetTimestampNanosNull) {
  arrow::Int64Builder builder;
  ASSERT_TRUE(builder.AppendNull().ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  EXPECT_EQ(getTimestampNanos(array, 0, arrow::Type::INT64), 0);
}

// --- getArrowValueRef ---

TEST(ParquetHelpersTest, GetArrowValueRefInt32) {
  arrow::Int32Builder builder;
  ASSERT_TRUE(builder.Append(42).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  auto val = getArrowValueRef(array, 0, arrow::Type::INT32);
  ASSERT_TRUE(std::holds_alternative<int32_t>(val));
  EXPECT_EQ(std::get<int32_t>(val), 42);
}

TEST(ParquetHelpersTest, GetArrowValueRefDouble) {
  arrow::DoubleBuilder builder;
  ASSERT_TRUE(builder.Append(3.14159).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  auto val = getArrowValueRef(array, 0, arrow::Type::DOUBLE);
  ASSERT_TRUE(std::holds_alternative<double>(val));
  EXPECT_DOUBLE_EQ(std::get<double>(val), 3.14159);
}

TEST(ParquetHelpersTest, GetArrowValueRefBool) {
  arrow::BooleanBuilder builder;
  ASSERT_TRUE(builder.Append(true).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  auto val = getArrowValueRef(array, 0, arrow::Type::BOOL);
  ASSERT_TRUE(std::holds_alternative<bool>(val));
  EXPECT_TRUE(std::get<bool>(val));
}

TEST(ParquetHelpersTest, GetArrowValueRefString) {
  arrow::StringBuilder builder;
  ASSERT_TRUE(builder.Append("hello world").ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  auto val = getArrowValueRef(array, 0, arrow::Type::STRING);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(val));
  EXPECT_EQ(std::get<std::string_view>(val), "hello world");
}

TEST(ParquetHelpersTest, GetArrowValueRefNull) {
  arrow::Int32Builder builder;
  ASSERT_TRUE(builder.AppendNull().ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());

  auto val = getArrowValueRef(array, 0, arrow::Type::INT32);
  ASSERT_TRUE(std::holds_alternative<PJ::NullValue>(val));
}

}  // namespace
