#include "arrow_series_reader.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/util/bit_util.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;

void requireOk(const arrow::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
}

template <typename T>
T valueOrThrow(arrow::Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(result.status().ToString());
  }
  return std::move(result).ValueUnsafe();
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> makeOptionalArray(const std::vector<std::optional<Value>>& values) {
  Builder builder;
  for (const auto& value : values) {
    if (value.has_value()) {
      requireOk(builder.Append(*value));
    } else {
      requireOk(builder.AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> result;
  requireOk(builder.Finish(&result));
  return result;
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> makePresentArray(const std::vector<Value>& values) {
  Builder builder;
  for (const auto value : values) {
    requireOk(builder.Append(value));
  }
  std::shared_ptr<arrow::Array> result;
  requireOk(builder.Finish(&result));
  return result;
}

std::shared_ptr<arrow::Array> makeStringArray(const std::vector<std::optional<std::string>>& values) {
  arrow::StringBuilder builder;
  for (const auto& value : values) {
    if (value.has_value()) {
      requireOk(builder.Append(value->data(), static_cast<int32_t>(value->size())));
    } else {
      requireOk(builder.AppendNull());
    }
  }
  std::shared_ptr<arrow::Array> result;
  requireOk(builder.Finish(&result));
  return result;
}

std::shared_ptr<arrow::Array> makeLargeStringArray(const std::vector<std::string>& values) {
  arrow::LargeStringBuilder builder;
  for (const auto& value : values) {
    requireOk(builder.Append(value.data(), static_cast<int64_t>(value.size())));
  }
  std::shared_ptr<arrow::Array> result;
  requireOk(builder.Finish(&result));
  return result;
}

std::shared_ptr<arrow::StructArray> makeStructArray(
    std::vector<std::shared_ptr<arrow::Array>> children, std::shared_ptr<arrow::Buffer> validity = nullptr,
    int64_t null_count = 0) {
  std::vector<std::string> names;
  names.reserve(children.size());
  for (size_t index = 0; index < children.size(); ++index) {
    names.push_back("ignored_" + std::to_string(index));
  }
  return valueOrThrow(arrow::StructArray::Make(children, names, std::move(validity), null_count));
}

std::shared_ptr<arrow::Buffer> makeValidityBitmap(const std::vector<bool>& validity) {
  auto bitmap = valueOrThrow(arrow::AllocateEmptyBitmap(static_cast<int64_t>(validity.size())));
  for (size_t index = 0; index < validity.size(); ++index) {
    if (validity[index]) {
      arrow::bit_util::SetBit(bitmap->mutable_data(), static_cast<int64_t>(index));
    }
  }
  return bitmap;
}

void exportArrayAndSchema(
    const std::shared_ptr<arrow::Array>& source, struct ArrowSchema* schema, struct ArrowArray* array) {
  requireOk(arrow::ExportArray(*source, array));
  if (source->type_id() == arrow::Type::STRUCT) {
    const auto struct_type = std::static_pointer_cast<arrow::StructType>(source->type());
    requireOk(arrow::ExportSchema(*arrow::schema(struct_type->fields()), schema));
  } else {
    requireOk(arrow::ExportType(*source->type(), schema));
  }
}

std::shared_ptr<arrow::Array> sequentialTimestamps(size_t count, int64_t first = 0) {
  std::vector<int64_t> timestamps;
  timestamps.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    timestamps.push_back(first + static_cast<int64_t>(index) * kNanosecondsPerSecond);
  }
  return makePresentArray<arrow::Int64Builder>(timestamps);
}

template <typename Builder, typename Value>
void expectNumericTypeDecodes(const std::string& type_name, const std::vector<Value>& values) {
  SCOPED_TRACE(type_name);
  const auto root = makeStructArray({sequentialTimestamps(values.size()), makePresentArray<Builder>(values)});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, type_name);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(schema.release, nullptr);
  EXPECT_EQ(array.release, nullptr);
  EXPECT_EQ(decoded->name, type_name);
  ASSERT_EQ(decoded->v.size(), values.size());
  for (size_t index = 0; index < values.size(); ++index) {
    EXPECT_DOUBLE_EQ(decoded->v[index], static_cast<double>(values[index]));
  }
}

}  // namespace

TEST(ArrowSeriesReader, NumericValueNullsDoNotCreatePhantomSamples) {
  const auto timestamps = sequentialTimestamps(5);
  const auto values = makeOptionalArray<arrow::DoubleBuilder, double>({10.0, std::nullopt, 30.0, std::nullopt, 50.0});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "numbers");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->name, "numbers");
  EXPECT_EQ(decoded->t, (std::vector<double>{0.0, 2.0, 4.0}));
  EXPECT_EQ(decoded->v, (std::vector<double>{10.0, 30.0, 50.0}));
  EXPECT_EQ(schema.release, nullptr);
  EXPECT_EQ(array.release, nullptr);
}

TEST(ArrowSeriesReader, Utf8NullsEmbeddedNulAndEmptyValueRoundTrip) {
  const std::string embedded_nul("ab\0cd", 5);
  const auto values = makeStringArray({"first", std::nullopt, embedded_nul, "", std::nullopt});
  const auto root = makeStructArray({sequentialTimestamps(5), values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeStringSeries(&schema, &array, "text");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->name, "text");
  EXPECT_EQ(decoded->t, (std::vector<double>{0.0, 2.0, 3.0}));
  ASSERT_EQ(decoded->v.size(), 3U);
  EXPECT_EQ(decoded->v[0], "first");
  EXPECT_EQ(decoded->v[1], embedded_nul);
  EXPECT_EQ(decoded->v[1].size(), 5U);
  EXPECT_TRUE(decoded->v[2].empty());
  EXPECT_EQ(schema.release, nullptr);
  EXPECT_EQ(array.release, nullptr);
}

TEST(ArrowSeriesReader, EverySupportedNumericTypeDecodes) {
  expectNumericTypeDecodes<arrow::DoubleBuilder>("float64", std::vector<double>{1.5, -3.0});
  expectNumericTypeDecodes<arrow::FloatBuilder>("float32", std::vector<float>{1.25F, -2.5F});
  expectNumericTypeDecodes<arrow::Int8Builder>("int8", std::vector<int8_t>{-8, 7});
  expectNumericTypeDecodes<arrow::Int16Builder>("int16", std::vector<int16_t>{-1600, 1500});
  expectNumericTypeDecodes<arrow::Int32Builder>("int32", std::vector<int32_t>{-32000, 31000});
  expectNumericTypeDecodes<arrow::Int64Builder>("int64", std::vector<int64_t>{-64'000, 63'000});
  expectNumericTypeDecodes<arrow::UInt8Builder>("uint8", std::vector<uint8_t>{8, 250});
  expectNumericTypeDecodes<arrow::UInt16Builder>("uint16", std::vector<uint16_t>{16, 65'000});
  expectNumericTypeDecodes<arrow::UInt32Builder>("uint32", std::vector<uint32_t>{32, 4'000'000'000U});
  expectNumericTypeDecodes<arrow::UInt64Builder>("uint64", std::vector<uint64_t>{64, 9'000'000'000ULL});
  expectNumericTypeDecodes<arrow::BooleanBuilder>("bool", std::vector<bool>{false, true});
}

TEST(ArrowSeriesReader, DecoderTypeMismatchesAreRejected) {
  {
    const auto root = makeStructArray({sequentialTimestamps(1), makeStringArray({"not numeric"})});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeNumericSeries(&schema, &array, "wrong").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
  {
    const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{1.0});
    const auto root = makeStructArray({sequentialTimestamps(1), values});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeStringSeries(&schema, &array, "wrong").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
  {
    const auto root = makeStructArray({sequentialTimestamps(1), makeLargeStringArray({"large utf8"})});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeStringSeries(&schema, &array, "wrong").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
}

TEST(ArrowSeriesReader, SlicedStructHonorsOffsetAndLength) {
  std::vector<double> values;
  values.reserve(10);
  for (size_t index = 0; index < 10; ++index) {
    values.push_back(100.0 + static_cast<double>(index));
  }
  const auto root = makeStructArray({sequentialTimestamps(10), makePresentArray<arrow::DoubleBuilder>(values)});
  const auto slice = root->Slice(3, 4);
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(slice, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "slice");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->t, (std::vector<double>{3.0, 4.0, 5.0, 6.0}));
  EXPECT_EQ(decoded->v, (std::vector<double>{103.0, 104.0, 105.0, 106.0}));
}

TEST(ArrowSeriesReader, IndependentlySlicedChildrenHonorTheirOffsets) {
  std::vector<double> values;
  values.reserve(10);
  for (size_t index = 0; index < 10; ++index) {
    values.push_back(200.0 + static_cast<double>(index));
  }
  const auto timestamps = sequentialTimestamps(10)->Slice(4, 3);
  const auto value_slice = makePresentArray<arrow::DoubleBuilder>(values)->Slice(4, 3);
  const auto root = makeStructArray({timestamps, value_slice});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "child slice");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->t, (std::vector<double>{4.0, 5.0, 6.0}));
  EXPECT_EQ(decoded->v, (std::vector<double>{204.0, 205.0, 206.0}));
}

TEST(ArrowSeriesReader, NullTimestampIsSkippedEvenWhenValueIsPresent) {
  const auto timestamps = makeOptionalArray<arrow::Int64Builder, int64_t>({0, std::nullopt, 2 * kNanosecondsPerSecond});
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{10.0, 20.0, 30.0});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "timestamp validity");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->t, (std::vector<double>{0.0, 2.0}));
  EXPECT_EQ(decoded->v, (std::vector<double>{10.0, 30.0}));
}

TEST(ArrowSeriesReader, NullStructRowIsSkippedEvenWhenChildrenArePresent) {
  const auto timestamps = sequentialTimestamps(3);
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{10.0, 20.0, 30.0});
  const auto root = makeStructArray({timestamps, values}, makeValidityBitmap({true, false, true}), 1);
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "struct validity");

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->t, (std::vector<double>{0.0, 2.0}));
  EXPECT_EQ(decoded->v, (std::vector<double>{10.0, 30.0}));
}

TEST(ArrowSeriesReader, EmptyAndAllNullArraysAreValidEmptySeries) {
  {
    const auto timestamps = makePresentArray<arrow::Int64Builder>(std::vector<int64_t>{});
    const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{});
    const auto root = makeStructArray({timestamps, values});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    auto decoded = decodeNumericSeries(&schema, &array, "empty");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->t.empty());
    EXPECT_TRUE(decoded->v.empty());
  }
  {
    const auto values = makeOptionalArray<arrow::DoubleBuilder, double>({std::nullopt, std::nullopt, std::nullopt});
    const auto root = makeStructArray({sequentialTimestamps(3), values});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    auto decoded = decodeNumericSeries(&schema, &array, "all null");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->t.empty());
    EXPECT_TRUE(decoded->v.empty());
  }
}

TEST(ArrowSeriesReader, StructurallyInvalidInputsAreRejected) {
  {
    const auto root = sequentialTimestamps(2);
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeNumericSeries(&schema, &array, "non-struct").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
  {
    const auto root = makeStructArray({sequentialTimestamps(2)});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeNumericSeries(&schema, &array, "one child").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
  {
    const auto wrong_timestamps = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{0.0, 1.0});
    const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{10.0, 20.0});
    const auto root = makeStructArray({wrong_timestamps, values});
    ArrowSchema schema{};
    ArrowArray array{};
    exportArrayAndSchema(root, &schema, &array);
    EXPECT_FALSE(decodeNumericSeries(&schema, &array, "wrong timestamp").has_value());
    EXPECT_EQ(schema.release, nullptr);
    EXPECT_EQ(array.release, nullptr);
  }
}

TEST(ArrowSeriesReader, NanosecondsConvertToSecondsAtLargeEpoch) {
  constexpr int64_t timestamp_ns = 1'750'000'000'123'456'789;
  const auto timestamps = makePresentArray<arrow::Int64Builder>(std::vector<int64_t>{timestamp_ns});
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{42.0});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "epoch");

  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->t.size(), 1U);
  EXPECT_DOUBLE_EQ(decoded->t[0], static_cast<double>(timestamp_ns) * 1e-9);
}

TEST(ArrowSeriesReader, ImportFailureConsumesBothExportedStructs) {
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{42.0});
  const auto root = makeStructArray({sequentialTimestamps(1), values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);
  schema.format = "not-an-arrow-format";

  EXPECT_FALSE(decodeNumericSeries(&schema, &array, "invalid import").has_value());
  EXPECT_EQ(schema.release, nullptr);
  EXPECT_EQ(array.release, nullptr);
}

// The host emits sealed chunks in commit order, so out-of-order streaming
// ingest can produce a series that steps backwards at chunk boundaries.
TEST(ArrowSeriesReader, NonMonotonicNumericTimestampsAreSortedOnDecode) {
  const std::vector<int64_t> unsorted_ns{10, 20, 5, 20, 15};
  const auto timestamps = makePresentArray<arrow::Int64Builder>(unsorted_ns);
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "unsorted");

  ASSERT_TRUE(decoded.has_value());
  const std::vector<int64_t> expected_ns{5, 10, 15, 20, 20};
  // The duplicate ts=20 rows must keep delivery order: 2.0 before 4.0.
  const std::vector<double> expected_v{3.0, 1.0, 5.0, 2.0, 4.0};
  ASSERT_EQ(decoded->t.size(), expected_ns.size());
  for (size_t index = 0; index < expected_ns.size(); ++index) {
    EXPECT_DOUBLE_EQ(decoded->t[index], static_cast<double>(expected_ns[index]) * 1e-9);
  }
  EXPECT_EQ(decoded->v, expected_v);
}

TEST(ArrowSeriesReader, NonMonotonicStringTimestampsAreSortedOnDecode) {
  const auto timestamps = makePresentArray<arrow::Int64Builder>(std::vector<int64_t>{10, 20, 5, 20, 15});
  const auto values = makeStringArray({"a", "b", "c", "d", "e"});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeStringSeries(&schema, &array, "unsorted strings");

  ASSERT_TRUE(decoded.has_value());
  const std::vector<std::string> expected_v{"c", "a", "e", "b", "d"};
  EXPECT_EQ(decoded->v, expected_v);
  EXPECT_TRUE(std::is_sorted(decoded->t.begin(), decoded->t.end()));
}

TEST(ArrowSeriesReader, SortedTimestampsWithDuplicatesPassThroughUnchanged) {
  const auto timestamps = makePresentArray<arrow::Int64Builder>(std::vector<int64_t>{10, 20, 20, 30});
  const auto values = makePresentArray<arrow::DoubleBuilder>(std::vector<double>{1.0, 2.0, 3.0, 4.0});
  const auto root = makeStructArray({timestamps, values});
  ArrowSchema schema{};
  ArrowArray array{};
  exportArrayAndSchema(root, &schema, &array);

  auto decoded = decodeNumericSeries(&schema, &array, "sorted with dups");

  ASSERT_TRUE(decoded.has_value());
  const std::vector<double> expected_v{1.0, 2.0, 3.0, 4.0};
  EXPECT_EQ(decoded->v, expected_v);
}
