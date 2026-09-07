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
#include <gtest/gtest.h>

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

}  // namespace
