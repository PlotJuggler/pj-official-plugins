// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// arrow-ipc framing: timestamp-field detection, the message host timestamp,
// and the one-batch-per-stream round trip `parser_arrow` decodes.

#include "../src/arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using mosaico::EmptyNameRule;
constexpr EmptyNameRule kIndex = EmptyNameRule::kIndex;
constexpr EmptyNameRule kFlatten = EmptyNameRule::kFlatten;

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> arrayOf(const std::vector<Value>& values) {
  Builder builder;
  for (const auto& value : values) {
    EXPECT_TRUE(builder.Append(value).ok());
  }
  std::shared_ptr<arrow::Array> array;
  EXPECT_TRUE(builder.Finish(&array).ok());
  return array;
}

std::shared_ptr<arrow::RecordBatch> scalarBatch() {
  auto schema = arrow::schema({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  return arrow::RecordBatch::Make(
      schema, 3,
      {arrayOf<arrow::Int64Builder, std::int64_t>({1000, 1001, 1002}),
       arrayOf<arrow::DoubleBuilder, double>({0.0, 1.0, 2.0})});
}

TEST(DetectTimestampLeaf, TypeThenNamePriorityThenEmpty) {
  auto typed_schema = arrow::schema(
      {arrow::field("timestamp_ns", arrow::int64()), arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MICRO))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*typed_schema, kIndex).path, "stamp");

  auto name_schema = arrow::schema(
      {arrow::field("ts", arrow::int64()), arrow::field("time", arrow::int64()),
       arrow::field("recording_timestamp_ns", arrow::int64())});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*name_schema, kIndex).path, "recording_timestamp_ns");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*arrow::schema({arrow::field("time", arrow::int64())}), kIndex).path, "time");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*arrow::schema({arrow::field("ts", arrow::int64())}), kIndex).path, "ts");

  EXPECT_EQ(mosaico::detectTimestampLeaf(*arrow::schema({arrow::field("x", arrow::float64())}), kIndex).path, "");
}

// A name match must also be a type parser_arrow would accept as an axis. Naming
// a utf8 `time` column instead makes the parser refuse the topic outright; left
// undetected it imports on the fitted synthetic cadence.
TEST(DetectTimestampLeaf, NameMatchNeedsAPlausibleAxisType) {
  auto named = [](const std::shared_ptr<arrow::DataType>& type, const char* name = "time") {
    return mosaico::detectTimestampLeaf(*arrow::schema({arrow::field(name, type)}), kIndex).path;
  };
  EXPECT_EQ(named(arrow::utf8()), "");
  EXPECT_EQ(named(arrow::int8(), "ts"), "");
  EXPECT_EQ(named(arrow::float32()), "") << "parser_arrow marks only float64 plausible";
  EXPECT_EQ(named(arrow::boolean()), "");

  // The four plausible types (SDK timestamp eligibility at nanosecond units).
  EXPECT_EQ(named(arrow::int64()), "time");
  EXPECT_EQ(named(arrow::uint64(), "ts"), "ts");
  EXPECT_EQ(named(arrow::float64(), "timestamp"), "timestamp");
  EXPECT_EQ(named(arrow::timestamp(arrow::TimeUnit::NANO)), "time");

  // Detection BY TYPE is ungated by the name list and unchanged by all this.
  EXPECT_EQ(named(arrow::timestamp(arrow::TimeUnit::MICRO), "whenever"), "whenever");
}

// The stamp a ROS-shaped topic carries lives inside its `header` struct: the
// scan walks flattened leaves, so it is found and named the way the flattened
// table (and parser_arrow) will name it.
TEST(DetectTimestampLeaf, WalksFlattenedLeavesAndNormalizesDots) {
  auto nested = arrow::schema(
      {arrow::field("value", arrow::float64()),
       arrow::field(
           "header", arrow::struct_(
                         {arrow::field("frame_id", arrow::utf8()),
                          arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MICRO))}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*nested, kIndex).path, "header/stamp");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*nested, kIndex).route, (std::vector<int>{1, 1}));

  // The name list matches a nested leaf by its full path, not its bare name.
  auto nested_by_name =
      arrow::schema({arrow::field("msg", arrow::struct_({arrow::field("timestamp_ns", arrow::int64())}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*nested_by_name, kIndex).path, "");

  // A '.' inside any component becomes '/' — at the top level…
  auto dotted_top = arrow::schema({arrow::field("wheel.stamp", arrow::timestamp(arrow::TimeUnit::NANO))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*dotted_top, kIndex).path, "wheel/stamp");

  // …and nested, where it composes with the struct separator.
  auto dotted_nested = arrow::schema(
      {arrow::field("wheel.speed", arrow::float64()),
       arrow::field("msg", arrow::struct_({arrow::field("header.stamp", arrow::timestamp(arrow::TimeUnit::NANO))}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*dotted_nested, kIndex).path, "msg/header/stamp");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*dotted_nested, kIndex).route, (std::vector<int>{1, 0}));

  // Nothing matches -> no path AND no route, which is how route() marks a
  // topic timestamp-less.
  EXPECT_TRUE(mosaico::detectTimestampLeaf(*nested_by_name, kIndex).route.empty());
}

// parser_arrow names an unnamed child `_<index>`. Mirroring it keeps the scalar
// contract exact. Object columns retain empty path components from Table::Flatten.
TEST(DetectTimestampLeaf, EmptyNameComponentsBecomeUnderscoreIndex) {
  auto unnamed_child = arrow::schema(
      {arrow::field("value", arrow::float64()),
       arrow::field("header", arrow::struct_({arrow::field("", arrow::timestamp(arrow::TimeUnit::MICRO))}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kIndex).path, "header/_0");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kIndex).route, (std::vector<int>{1, 0}));

  auto unnamed_top = arrow::schema({arrow::field("", arrow::timestamp(arrow::TimeUnit::NANO))});
  const auto leaf = mosaico::detectTimestampLeaf(*unnamed_top, kIndex);
  EXPECT_EQ(leaf.path, "_0");
  EXPECT_FALSE(leaf.route.empty());

  // The object route's consumer is Table::Flatten, which writes a trailing
  // separator for an unnamed child instead. Same walk, stated rule.
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kFlatten).path, "header/");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kFlatten).route, (std::vector<int>{1, 0}));
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_top, kFlatten).path, "");
  auto unnamed_parent = arrow::schema(
      {arrow::field("", arrow::struct_({arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::NANO))}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_parent, kFlatten).path, "/stamp");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_parent, kIndex).path, "_0/stamp");
}

TEST(FirstRowTimestampNs, ByTypeAndInvalidRoutes) {
  EXPECT_EQ(mosaico::firstRowTimestampNs(*scalarBatch(), {0}), 1000);

  auto double_schema = arrow::schema({arrow::field("t", arrow::float64())});
  auto double_batch = arrow::RecordBatch::Make(double_schema, 1, {arrayOf<arrow::DoubleBuilder, double>({1.5})});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*double_batch, {0}), 1'500'000'000LL);

  // Epoch-scale witnesses: a `long double` product at 1e18 carries a 0.125 ns
  // ulp, so ~6% of real timestamps round a whole nanosecond away from the split
  // conversion parser_arrow does. Each expectation below is the SPLIT result;
  // the old long-double form returns one nanosecond further from zero.
  auto stamped = [&](double seconds) {
    return mosaico::firstRowTimestampNs(
        *arrow::RecordBatch::Make(double_schema, 1, {arrayOf<arrow::DoubleBuilder, double>({seconds})}), {0});
  };
  EXPECT_EQ(stamped(1620132785.1132183), 1'620'132'785'113'218'307LL);
  EXPECT_EQ(stamped(1702807114.4990616), 1'702'807'114'499'061'584LL);
  EXPECT_EQ(stamped(-1529583318.5568106), -1'529'583'318'556'810'617LL);

  // Shared with parser_arrow's ConvertsFloatingSecondsWithPortableIntegerArithmetic:
  // both suites pin the same two values, so the twin cannot drift silently.
  EXPECT_EQ(stamped(1'700'000'000.125), 1'700'000'000'125'000'000LL);
  EXPECT_EQ(stamped(-1.6e-9), -2LL);

  // Half-float is refused: parser_arrow rejects it as an axis, so stamping from
  // it here would disagree with the parser about the very same column.
  arrow::HalfFloatBuilder half_builder;
  ASSERT_TRUE(half_builder.Append(static_cast<std::uint16_t>(0x3C00)).ok());  // 1.0
  std::shared_ptr<arrow::Array> half_array;
  ASSERT_TRUE(half_builder.Finish(&half_array).ok());
  auto half_batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("time", arrow::float16())}), 1, {half_array});
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*half_batch, {0}).has_value());

  auto milli_schema = arrow::schema({arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MILLI))});
  arrow::TimestampBuilder milli_builder(arrow::timestamp(arrow::TimeUnit::MILLI), arrow::default_memory_pool());
  ASSERT_TRUE(milli_builder.Append(7).ok());
  std::shared_ptr<arrow::Array> milli_array;
  ASSERT_TRUE(milli_builder.Finish(&milli_array).ok());
  auto milli_batch = arrow::RecordBatch::Make(milli_schema, 1, {milli_array});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*milli_batch, {0}), 7'000'000LL);

  // An empty route is what route() stores for a topic with no timestamp column.
  auto batch = scalarBatch();
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, {}).has_value());
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, {batch->num_columns()}).has_value());
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch->Slice(0, 0), {0}).has_value());
}

// A nested stamp is read through its child-index route, so the message host
// timestamp is the row's real time rather than a synthetic one.
TEST(FirstRowTimestampNs, DescendsAStructRoute) {
  arrow::TimestampBuilder stamp_builder(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
  ASSERT_TRUE(stamp_builder.Append(1'234).ok());
  std::shared_ptr<arrow::Array> stamps;
  ASSERT_TRUE(stamp_builder.Finish(&stamps).ok());
  auto header = *arrow::StructArray::Make(
      arrow::ArrayVector{arrayOf<arrow::StringBuilder, std::string>({"map"}), stamps},
      std::vector<std::string>{"frame_id", "stamp"});
  auto batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("header", header->type())}), 1, {std::static_pointer_cast<arrow::Array>(header)});

  EXPECT_EQ(mosaico::firstRowTimestampNs(*batch, {0, 1}), 1'234'000LL);
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, {0, 0}).has_value()) << "frame_id is not a timestamp";
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, {0, 5}).has_value());
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, {0, 1, 0}).has_value()) << "a scalar leaf has no children";
}

TEST(SerializeIpcStream, RoundTripsOneBatchAsACompleteStream) {
  auto batch = scalarBatch();
  auto bytes = mosaico::serializeIpcStream(*batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();

  auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(*bytes));
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  EXPECT_TRUE((*reader)->schema()->Equals(*batch->schema()));
  auto decoded = (*reader)->Next();
  ASSERT_TRUE(decoded.ok());
  ASSERT_NE(*decoded, nullptr);
  EXPECT_TRUE((*decoded)->Equals(*batch));
  auto end = (*reader)->Next();
  ASSERT_TRUE(end.ok());
  EXPECT_EQ(*end, nullptr) << "exactly one batch then end-of-stream";
}

TEST(ParserConfigJson, CarriesEveryKeyTheContractDependsOn) {
  auto config = nlohmann::json::parse(mosaico::parserConfigJson("header/stamp", mosaico::kSyntheticIntervalNs));
  EXPECT_EQ(config.at("timestamp_column"), "header/stamp");
  EXPECT_EQ(config.at("synthetic_interval_ns"), mosaico::kSyntheticIntervalNs);
  // Pinned, not left to the parser's default: a nested timestamp_column only
  // resolves once the parser has flattened.
  EXPECT_EQ(config.at("flatten_structs"), true);

  // The interval is per-topic: a fitted cadence must reach the parser verbatim.
  auto fitted = nlohmann::json::parse(mosaico::parserConfigJson("", 1000));
  EXPECT_EQ(fitted.at("timestamp_column"), "");
  EXPECT_EQ(fitted.at("synthetic_interval_ns"), 1000);
}

std::shared_ptr<arrow::RecordBatch> decodeSingleBatch(const std::shared_ptr<arrow::Buffer>& bytes) {
  auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(bytes));
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  auto batch = (*reader)->Next();
  EXPECT_TRUE(batch.ok());
  return *batch;
}

TEST(SerializeIpcStream, PreservesViewsDictionariesRunEndsAndMetadata) {
  auto dictionary = arrow::DictionaryArray::FromArrays(
      arrayOf<arrow::Int8Builder, std::int8_t>({1, 0}), arrayOf<arrow::StringBuilder, std::string>({"left", "right"}));
  ASSERT_TRUE(dictionary.ok());
  auto ree = arrow::RunEndEncodedArray::Make(
      2, arrayOf<arrow::Int16Builder, std::int16_t>({2}), arrayOf<arrow::Int64Builder, std::int64_t>({42}));
  ASSERT_TRUE(ree.ok());
  auto view = arrayOf<arrow::StringViewBuilder, std::string>({"first", "second"});
  auto metadata = arrow::key_value_metadata({"source"}, {"untouched"});
  auto schema = arrow::schema(
      {arrow::field("label", (*dictionary)->type(), true, metadata), arrow::field("counter", (*ree)->type()),
       arrow::field("view", view->type())},
      metadata);
  auto batch = arrow::RecordBatch::Make(schema, 2, {*dictionary, *ree, view});
  auto bytes = mosaico::serializeIpcStream(*batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status();
  auto decoded = decodeSingleBatch(*bytes);
  EXPECT_TRUE(decoded->Equals(*batch, true));
}

TEST(SerializeIpcStream, StoresSyntheticTimingWithoutOverwritingSourceFields) {
  auto values = arrayOf<arrow::Int64Builder, std::int64_t>({9, 8, 7});
  auto schema = arrow::schema({arrow::field("__mosaico_timestamp", values->type())});
  auto batch = arrow::RecordBatch::Make(schema, 3, {values});
  auto timed = mosaico::addSyntheticTimestamps(*batch, 100, 400, 0);
  ASSERT_TRUE(timed.ok()) << timed.status();
  auto bytes = mosaico::serializeIpcStream(**timed);
  ASSERT_TRUE(bytes.ok());
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_EQ(decoded->num_columns(), 2);
  EXPECT_EQ(decoded->column_name(0), "__mosaico_timestamp_");
  EXPECT_TRUE(decoded->column(1)->Equals(values));
  const auto stamps = std::static_pointer_cast<arrow::TimestampArray>(decoded->column(0));
  EXPECT_EQ(stamps->Value(0), 100);
  EXPECT_EQ(stamps->Value(1), 500);
  EXPECT_EQ(stamps->Value(2), 900);
  EXPECT_EQ(mosaico::detectTimestampLeaf(*decoded->schema(), kIndex).route, (std::vector<int>{0}));
  EXPECT_FALSE(mosaico::addSyntheticTimestamps(*batch, std::numeric_limits<int64_t>::max(), 1, 0).ok());
  EXPECT_FALSE(mosaico::addSyntheticTimestamps(*batch, 0, 0, std::numeric_limits<int64_t>::max()).ok());
}

TEST(DetectTimestampLeaf, UsesCanonicalNamesAndUnitDependentEligibility) {
  for (const auto rule : {kIndex, kFlatten}) {
    for (const auto name : {"TIME", "t", "time_stamp", "datetime", "date_time", "_timestamp", "_time"}) {
      const auto schema = arrow::schema({arrow::field(name, arrow::int64())});
      EXPECT_EQ(mosaico::detectTimestampLeaf(*schema, rule).path, name);
    }
    const auto schema = arrow::schema({arrow::field("time", arrow::int32())});
    EXPECT_TRUE(mosaico::detectTimestampLeaf(*schema, rule).path.empty());
    EXPECT_EQ(mosaico::detectTimestampLeaf(*schema, rule, PJ::TimeUnit::kSeconds).path, "time");
  }
}

TEST(FirstRowTimestampNs, ConfiguredIntegerUnitsAndOverflow) {
  const auto batch = scalarBatch();
  EXPECT_EQ(mosaico::firstRowTimestampNs(*batch, {0}, PJ::TimeUnit::kMicroseconds), 1'000'000);
  auto large = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("time", arrow::uint64())}), 1,
      {arrayOf<arrow::UInt64Builder, std::uint64_t>({std::numeric_limits<std::uint64_t>::max()})});
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*large, {0}));
  const auto config = nlohmann::json::parse(mosaico::parserConfigJson("time", 0, PJ::TimeUnit::kMicroseconds));
  EXPECT_EQ(config.at("timestamp_unit"), "us");
}

TEST(NormalizeViewColumns, CollapsesPerRowSliceAndRoundTrips) {
  // Three ~1 MB values force out-of-line variadic buffers in the view array.
  const std::string big(1u << 20, 'x');
  arrow::BinaryViewBuilder builder;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(builder.Append(big).ok());
  }
  std::shared_ptr<arrow::Array> views;
  ASSERT_TRUE(builder.Finish(&views).ok());
  auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("data", arrow::binary_view())}), 3, {views});

  // Arrow's IPC writer emits a view array's whole variadic buffers for a 1-row
  // slice, so one row carries the batch's ~3 MB.
  auto raw = mosaico::serializeIpcStream(*batch->Slice(1, 1));
  ASSERT_TRUE(raw.ok());
  EXPECT_GT((*raw)->size(), static_cast<std::int64_t>(2) << 20);

  // De-viewed, a 1-row slice carries only its own ~1 MB, and decodes to binary.
  auto normalized = mosaico::normalizeViewColumns(*batch);
  ASSERT_TRUE(normalized.ok());
  ASSERT_EQ((*normalized)->schema()->field(0)->type()->id(), arrow::Type::LARGE_BINARY);
  auto slim = mosaico::serializeIpcStream(*(*normalized)->Slice(1, 1));
  ASSERT_TRUE(slim.ok());
  EXPECT_LT((*slim)->size(), (static_cast<std::int64_t>(1) << 20) + (8 << 10));

  auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(*slim));
  ASSERT_TRUE(reader.ok());
  std::shared_ptr<arrow::RecordBatch> decoded;
  ASSERT_TRUE((*reader)->ReadNext(&decoded).ok());
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(std::static_pointer_cast<arrow::LargeBinaryArray>(decoded->column(0))->GetString(0), big);
}

TEST(NormalizeViewColumns, NoViewColumnsReturnsUnchanged) {
  const auto batch = scalarBatch();
  auto normalized = mosaico::normalizeViewColumns(*batch);
  ASSERT_TRUE(normalized.ok());
  EXPECT_TRUE((*normalized)->schema()->Equals(*batch->schema()));
  EXPECT_TRUE((*normalized)->Equals(*batch));
}

TEST(NormalizeViewColumns, RewritesViewNestedInStruct) {
  arrow::StringViewBuilder inner_builder;
  ASSERT_TRUE(inner_builder.Append(std::string(4096, 'a')).ok());
  std::shared_ptr<arrow::Array> inner;
  ASSERT_TRUE(inner_builder.Finish(&inner).ok());
  auto encoded = arrow::DictionaryArray::FromArrays(arrayOf<arrow::Int8Builder, std::int8_t>({0}), inner);
  ASSERT_TRUE(encoded.ok());
  for (const auto& child : arrow::ArrayVector{inner, *encoded}) {
    auto struct_result = arrow::StructArray::Make({child}, std::vector<std::string>{"frame_id"});
    ASSERT_TRUE(struct_result.ok());
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("header", (*struct_result)->type())}), 1, {*struct_result});
    auto normalized = mosaico::normalizeViewColumns(*batch);
    ASSERT_TRUE(normalized.ok()) << normalized.status();
    const auto output = std::static_pointer_cast<arrow::StructArray>((*normalized)->column(0));
    ASSERT_EQ(output->field(0)->type_id(), arrow::Type::LARGE_STRING);
    EXPECT_EQ(
        std::static_pointer_cast<arrow::LargeStringArray>(output->field(0))->GetString(0), std::string(4096, 'a'));
  }
}

TEST(NormalizeViewColumns, RejectsMalformedViewsBeforeCasting) {
  auto views = arrayOf<arrow::BinaryViewBuilder, std::string>({std::string(4096, 'x')});
  auto* descriptor = reinterpret_cast<arrow::BinaryViewType::c_type*>(views->data()->buffers[1]->mutable_data());
  descriptor->ref.buffer_index = 1'000'000;
  auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("data", views->type())}), 1, {views});
  ASSERT_FALSE(batch->ValidateFull().ok());
  ASSERT_FALSE(mosaico::serializeIpcStream(*batch).ok());

  // IPC decoding alone does not validate a descriptor's buffer reference.
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto writer = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
  ASSERT_TRUE(writer->WriteRecordBatch(*batch).ok());
  ASSERT_TRUE(writer->Close().ok());
  auto incoming = decodeSingleBatch(sink->Finish().ValueOrDie());
  ASSERT_FALSE(incoming->ValidateFull().ok());
  // Isolate an unchecked cast's SIGSEGV so the remaining regressions still run.
  EXPECT_EXIT(
      {
        auto result = mosaico::normalizeViewColumns(*incoming);
        std::_Exit(result.status().IsInvalid() ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(NormalizeViewColumns, SharedViewBufferCanMaterializeBeyondInt32Offsets) {
  const std::string value(1u << 20, 'x');
  auto one = arrayOf<arrow::BinaryViewBuilder, std::string>({value});
  auto data = one->data()->Copy();
  constexpr std::int64_t kRows = 2048;
  // Only 1 MiB of backing storage, but 2 GiB of materialized values.
  data->buffers[1] = arrow::Buffer::FromVector(
      std::vector<arrow::BinaryViewType::c_type>(kRows, data->GetValues<arrow::BinaryViewType::c_type>(1)[0]));
  data->length = kRows;
  auto batch =
      arrow::RecordBatch::Make(arrow::schema({arrow::field("data", data->type)}), kRows, {arrow::MakeArray(data)});
  ASSERT_TRUE(batch->ValidateFull().ok());
  auto normalized = mosaico::normalizeViewColumns(*batch);
  ASSERT_TRUE(normalized.ok()) << normalized.status();
  ASSERT_TRUE((*normalized)->ValidateFull().ok()) << (*normalized)->ValidateFull();
  auto bytes = mosaico::serializeIpcStream(*(*normalized)->Slice(kRows - 1, 1));
  ASSERT_TRUE(bytes.ok()) << bytes.status();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_EQ(decoded->column(0)->type_id(), arrow::Type::LARGE_BINARY);
  EXPECT_EQ(std::static_pointer_cast<arrow::LargeBinaryArray>(decoded->column(0))->GetString(0), value);
}

TEST(NormalizeViewColumns, NestedAndEncodedViewsProduceCompactRowMessages) {
  constexpr std::int64_t kValueBytes = 64 << 10;
  auto values = arrayOf<arrow::BinaryViewBuilder, std::string>(
      {std::string(kValueBytes, 'a'), std::string(kValueBytes, 'b'), std::string(kValueBytes, 'c')});
  auto offsets = arrayOf<arrow::Int32Builder, std::int32_t>({0, 1, 2, 3});
  auto large_offsets = arrayOf<arrow::Int64Builder, std::int64_t>({0, 1, 2, 3});
  const arrow::ArrayVector cases{
      arrow::StructArray::Make({values}, std::vector<std::string>{"payload"}).ValueOrDie(),
      arrow::ListArray::FromArrays(*offsets, *values).ValueOrDie(),
      arrow::LargeListArray::FromArrays(*large_offsets, *values).ValueOrDie(),
      arrow::FixedSizeListArray::FromArrays(values, 1).ValueOrDie(),
      arrow::MapArray::FromArrays(offsets, offsets->Slice(0, 3), values).ValueOrDie(),
      arrow::DictionaryArray::FromArrays(offsets->Slice(0, 3), values).ValueOrDie(),
      arrow::ListViewArray::FromArrays(
          *offsets->Slice(0, 3), *arrayOf<arrow::Int32Builder, std::int32_t>({1, 1, 1}), *values)
          .ValueOrDie(),
      arrow::LargeListViewArray::FromArrays(
          *large_offsets->Slice(0, 3), *arrayOf<arrow::Int64Builder, std::int64_t>({1, 1, 1}), *values)
          .ValueOrDie()};
  for (const auto& column : cases) {
    SCOPED_TRACE(column->type()->ToString());
    auto batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("data", column->type())}), 3, {column});
    // Include a nonzero parent offset in the input to normalization.
    for (const auto& input : {batch, batch->Slice(1, 2)}) {
      auto normalized = mosaico::normalizeViewColumns(*input);
      ASSERT_TRUE(normalized.ok()) << normalized.status();
      ASSERT_TRUE((*normalized)->ValidateFull().ok()) << (*normalized)->ValidateFull();
      auto row = (*normalized)->Slice(1, 1);
      auto bytes = mosaico::serializeIpcStream(*row);
      ASSERT_TRUE(bytes.ok()) << bytes.status();
      EXPECT_LT((*bytes)->size(), kValueBytes + 8192);
      auto decoded = decodeSingleBatch(*bytes);
      EXPECT_TRUE(decoded->Equals(*row, true));
      auto source = input->column(0)->Slice(1, 1);
      if (column->type_id() == arrow::Type::DICTIONARY) {
        source = values->Slice(input == batch ? 1 : 2, 1);
      }
      auto expected = arrow::compute::Cast(*source, decoded->column(0)->type());
      ASSERT_TRUE(expected.ok()) << expected.status();
      EXPECT_TRUE(decoded->column(0)->Equals(*expected));
    }
  }
}

TEST(NormalizeViewColumns, PreservesMapFieldsIncludingWhenNoViewsExist) {
  auto metadata = arrow::key_value_metadata({"source"}, {"preserved"});
  auto keys = arrayOf<arrow::Int32Builder, std::int32_t>({0, 1, 2});
  auto offsets = arrayOf<arrow::Int32Builder, std::int32_t>({0, 1, 2, 3});
  for (const auto& values : {keys, arrayOf<arrow::StringViewBuilder, std::string>({"a", "b", "c"})}) {
    auto entries = arrow::field(
        "pairs",
        arrow::struct_(
            {arrow::field("key", keys->type(), false, metadata),
             arrow::field("value", values->type(), false, metadata)}),
        false, metadata);
    auto type = std::make_shared<arrow::MapType>(entries, true);
    auto column = arrow::MapArray::FromArrays(type, offsets, keys, values).ValueOrDie();
    auto batch =
        arrow::RecordBatch::Make(arrow::schema({arrow::field("data", type, false, metadata)}, metadata), 3, {column});
    auto normalized = mosaico::normalizeViewColumns(*batch);
    ASSERT_TRUE(normalized.ok()) << normalized.status();
    const auto& output = static_cast<const arrow::MapType&>(*(*normalized)->column(0)->type());
    EXPECT_TRUE(output.key_field()->Equals(type->key_field(), true));
    EXPECT_TRUE(output.item_field()->WithType(values->type())->Equals(type->item_field(), true));
    EXPECT_TRUE(output.value_field()->WithType(type->value_type())->Equals(entries, true));
    EXPECT_TRUE(output.keys_sorted());
    EXPECT_TRUE((*normalized)->schema()->metadata()->Equals(*metadata));
    EXPECT_TRUE((*normalized)->schema()->field(0)->WithType(type)->Equals(batch->schema()->field(0), true));
    if (values == keys) {
      EXPECT_TRUE((*normalized)->Equals(*batch, true));
      EXPECT_EQ((*normalized)->column(0)->data()->buffers, column->data()->buffers);
    }
  }
}

}  // namespace
