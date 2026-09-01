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
// contract exact, and keeps an unnamed top-level stamp from colliding with the
// empty string that means "this topic has no timestamp".
TEST(DetectTimestampLeaf, EmptyNameComponentsBecomeUnderscoreIndex) {
  auto unnamed_child = arrow::schema(
      {arrow::field("value", arrow::float64()),
       arrow::field("header", arrow::struct_({arrow::field("", arrow::timestamp(arrow::TimeUnit::MICRO))}))});
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kIndex).path, "header/_0");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kIndex).route, (std::vector<int>{1, 0}));

  auto unnamed_top = arrow::schema({arrow::field("", arrow::timestamp(arrow::TimeUnit::NANO))});
  const auto leaf = mosaico::detectTimestampLeaf(*unnamed_top, kIndex);
  EXPECT_EQ(leaf.path, "_0") << "not \"\", which route() reads as timestamp-less";
  EXPECT_FALSE(leaf.route.empty());

  // The object route's consumer is Table::Flatten, which writes a trailing
  // separator for an unnamed child instead. Same walk, stated rule.
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kFlatten).path, "header/");
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_child, kFlatten).route, (std::vector<int>{1, 0}));
  EXPECT_EQ(mosaico::detectTimestampLeaf(*unnamed_top, kFlatten).path, "");
}

TEST(FirstRowTimestampNs, ByTypeAndInvalidRoutes) {
  EXPECT_EQ(mosaico::firstRowTimestampNs(*scalarBatch(), {0}), 1000);

  auto double_schema = arrow::schema({arrow::field("t", arrow::float64())});
  auto double_batch = arrow::RecordBatch::Make(double_schema, 1, {arrayOf<arrow::DoubleBuilder, double>({1.5})});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*double_batch, {0}), 1'500'000'000LL);

  // Witness value: in plain double the product lands on a .5 tie and rounds a
  // whole nanosecond past what parser_arrow computes for the same column.
  auto witness_batch =
      arrow::RecordBatch::Make(double_schema, 1, {arrayOf<arrow::DoubleBuilder, double>({-362.3269081675})});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*witness_batch, {0}), -362'326'908'167LL);

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

std::shared_ptr<arrow::Array> castArray(
    const std::shared_ptr<arrow::Array>& array, const std::shared_ptr<arrow::DataType>& type) {
  auto casted = arrow::compute::Cast(*array, type);
  EXPECT_TRUE(casted.ok()) << casted.status().ToString();
  return *casted;
}

std::shared_ptr<arrow::RecordBatch> decodeSingleBatch(const std::shared_ptr<arrow::Buffer>& bytes) {
  auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(bytes));
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  auto batch = (*reader)->Next();
  EXPECT_TRUE(batch.ok());
  return *batch;
}

// nanoarrow_ipc (parser_arrow) cannot decode view types, even nested inside a
// struct, nor dictionary batches: the cast pass must remove all of them and the
// stream must decode to plain utf8/binary values.
TEST(IpcSafeSchema, ViewAndDictionaryFieldsBecomePlainTypesAtEveryDepth) {
  auto names = arrayOf<arrow::StringBuilder, std::string>({"map", "odom", "map"});
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto header = *arrow::StructArray::Make(
      {castArray(names, arrow::utf8_view()), ids}, std::vector<std::string>{"frame_id", "seq"});
  auto schema = arrow::schema(
      {arrow::field("frame_id", arrow::utf8_view()), arrow::field("blob", arrow::binary_view()),
       arrow::field("label", arrow::dictionary(arrow::int32(), arrow::utf8())), arrow::field("header", header->type()),
       arrow::field("x", arrow::int64())});
  auto batch = arrow::RecordBatch::Make(
      schema, 3,
      {castArray(names, arrow::utf8_view()), castArray(names, arrow::binary_view()),
       castArray(names, arrow::dictionary(arrow::int32(), arrow::utf8())), header, ids});

  auto safe_result = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe_result.ok()) << safe_result.status().ToString();
  const mosaico::IpcSafeSchema& safe = *safe_result;
  EXPECT_TRUE(safe.dropped.empty());
  auto safe_schema = safe.schema;
  ASSERT_NE(safe_schema, schema);
  EXPECT_TRUE(safe_schema->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(safe_schema->field(1)->type()->Equals(arrow::binary()));
  EXPECT_TRUE(safe_schema->field(2)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(safe_schema->field(3)->type()->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(safe_schema->field(4)->type()->Equals(arrow::int64()));

  auto safe_batch = mosaico::castToSchema(*batch, safe);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  auto bytes = mosaico::serializeIpcStream(**safe_batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(decoded->schema()->Equals(*safe_schema));
  EXPECT_EQ(decoded->column(0)->type_id(), arrow::Type::STRING);
  EXPECT_EQ(decoded->column(2)->type_id(), arrow::Type::STRING);
  EXPECT_TRUE(decoded->column(0)->Equals(*names));
  EXPECT_TRUE(decoded->column(2)->Equals(*names));
  EXPECT_TRUE(decoded->column(4)->Equals(*ids));
}

TEST(IpcSafeSchema, ReturnsTheSamePointerWhenNothingNeedsCasting) {
  auto schema = scalarBatch()->schema();
  auto safe = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe.ok()) << safe.status().ToString();
  EXPECT_EQ(safe->schema, schema);
  ASSERT_EQ(safe->columns.size(), 2U);
  EXPECT_EQ(safe->columns[0].source_index, 0);
  EXPECT_EQ(safe->columns[1].source_index, 1);
  EXPECT_TRUE(safe->dropped.empty());
}

// Every rewrite the allowlist promises must survive a REAL cast, not just a
// schema edit: an untested branch would only fail once a server emitted it. One
// column per rewrite kind, cast and then written and read back with LIBARROW's
// own IPC reader — which proves the casts and that the result is valid Arrow
// IPC, NOT that nanoarrow decodes it. That gate lives in parser_arrow's tests.
TEST(IpcSafeSchema, EveryRewriteKindActuallyCasts) {
  auto names = arrayOf<arrow::StringBuilder, std::string>({"a", "b", "c"});
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto views = castArray(names, arrow::utf8_view());
  auto dicts = castArray(names, arrow::dictionary(arrow::int32(), arrow::utf8()));
  auto offsets = arrayOf<arrow::Int32Builder, std::int32_t>({0, 1, 3});
  auto view_offsets = arrayOf<arrow::Int32Builder, std::int32_t>({0, 1});
  auto view_sizes = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2});
  auto large_offsets = arrayOf<arrow::Int64Builder, std::int64_t>({0, 1});
  auto large_sizes = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2});

  auto list_view = *arrow::ListViewArray::FromArrays(*view_offsets, *view_sizes, *ids);
  auto large_list_view = *arrow::LargeListViewArray::FromArrays(*large_offsets, *large_sizes, *ids);
  auto list_of_views = *arrow::ListArray::FromArrays(*offsets, *views);
  auto struct_of_dict = *arrow::StructArray::Make(arrow::ArrayVector{dicts}, std::vector<std::string>{"label"});
  auto map_of_views = *arrow::MapArray::FromArrays(offsets, names, views);
  auto fixed_of_views = *arrow::FixedSizeListArray::FromArrays(views, 1);

  // The list-shaped columns hold 2 rows; every column is sliced to that.
  arrow::ArrayVector columns;
  arrow::FieldVector fields;
  for (const auto& column : arrow::ArrayVector{
           views, castArray(names, arrow::binary_view()), dicts, list_view, large_list_view, list_of_views,
           struct_of_dict, map_of_views, fixed_of_views}) {
    fields.push_back(arrow::field("f" + std::to_string(fields.size()), column->type()));
    columns.push_back(column->Slice(0, 2));
  }
  auto batch = arrow::RecordBatch::Make(arrow::schema(fields), 2, columns);

  auto safe_schema = mosaico::ipcSafeSchema(batch->schema());
  ASSERT_TRUE(safe_schema.ok()) << safe_schema.status().ToString();
  EXPECT_TRUE((*safe_schema).schema->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE((*safe_schema).schema->field(1)->type()->Equals(arrow::binary()));
  EXPECT_TRUE((*safe_schema).schema->field(2)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE((*safe_schema).schema->field(3)->type()->Equals(arrow::list(arrow::field("item", arrow::int64()))));
  EXPECT_TRUE((*safe_schema).schema->field(4)->type()->Equals(arrow::large_list(arrow::field("item", arrow::int64()))));
  EXPECT_TRUE((*safe_schema).schema->field(5)->type()->Equals(arrow::list(arrow::field("item", arrow::utf8()))));
  EXPECT_TRUE((*safe_schema).schema->field(6)->type()->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE((*safe_schema).schema->field(7)->type()->Equals(arrow::map(arrow::utf8(), arrow::utf8())));
  EXPECT_TRUE((*safe_schema).schema->field(8)->type()->Equals(arrow::fixed_size_list(arrow::utf8(), 1)));

  auto safe_batch = mosaico::castToSchema(*batch, *safe_schema);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  auto bytes = mosaico::serializeIpcStream(**safe_batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(decoded->schema()->Equals(*(*safe_schema).schema));
}

// Stand-in for arrow.opaque / arrow.json / uuid / geoarrow: only the storage
// type matters to ipcSafeSchema, and a locally defined one needs no registry.
class TestExtensionType : public arrow::ExtensionType {
 public:
  explicit TestExtensionType(std::shared_ptr<arrow::DataType> storage) : arrow::ExtensionType(std::move(storage)) {}
  std::string extension_name() const override {
    return "mosaico.test";
  }
  bool ExtensionEquals(const arrow::ExtensionType& other) const override {
    return other.extension_name() == extension_name() && other.storage_type()->Equals(*storage_type());
  }
  std::shared_ptr<arrow::Array> MakeArray(std::shared_ptr<arrow::ArrayData> data) const override {
    return std::make_shared<arrow::ExtensionArray>(std::move(data));
  }
  arrow::Result<std::shared_ptr<arrow::DataType>> Deserialize(
      std::shared_ptr<arrow::DataType> storage_type, const std::string&) const override {
    return std::make_shared<TestExtensionType>(std::move(storage_type));
  }
  std::string Serialize() const override {
    return {};
  }
};

// Arrow frames an extension field as its storage type, so nanoarrow decodes it
// and main imported arrow.opaque / arrow.json / uuid / geoarrow fine — refusing
// them would be a regression. The storage still has to pass the allowlist.
TEST(IpcSafeSchema, ExtensionFieldsBecomeTheirStorageType) {
  auto names = arrayOf<arrow::StringBuilder, std::string>({"a", "b", "c"});
  std::shared_ptr<arrow::DataType> plain = std::make_shared<TestExtensionType>(arrow::utf8());
  std::shared_ptr<arrow::DataType> over_view = std::make_shared<TestExtensionType>(arrow::utf8_view());

  auto schema = arrow::schema({arrow::field("doc", plain), arrow::field("blob", over_view)});
  auto safe = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe.ok()) << safe.status().ToString();
  EXPECT_TRUE(safe->schema->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(safe->schema->field(1)->type()->Equals(arrow::utf8())) << "a view-typed storage is still cast away";

  auto batch = arrow::RecordBatch::Make(
      schema, 3,
      {arrow::ExtensionType::WrapArray(plain, names),
       arrow::ExtensionType::WrapArray(over_view, castArray(names, arrow::utf8_view()))});
  auto safe_batch = mosaico::castToSchema(*batch, *safe);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  auto bytes = mosaico::serializeIpcStream(**safe_batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(decoded->column(0)->Equals(*names));
  EXPECT_TRUE(decoded->column(1)->Equals(*names));
}

// Run-end encoding would materialize through arrow::compute::RunEndDecode, but
// the pinned Arrow is built with compute=False so that kernel is unregistered.
// It is therefore droppable, not fatal — and the diagnostic says why.
TEST(IpcSafeSchema, RunEndEncodedColumnsAreDroppedWithTheReason) {
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({2, 3});
  auto values = arrayOf<arrow::Int64Builder, std::int64_t>({7, 9});
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, values);
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});

  auto schema = arrow::schema({arrow::field("counter", ree->type()), arrow::field("x", arrow::int64())});
  auto safe = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe.ok()) << safe.status().ToString();
  ASSERT_EQ(safe->dropped.size(), 1U);
  EXPECT_NE(safe->dropped[0].find("'counter'"), std::string::npos) << safe->dropped[0];
  EXPECT_NE(safe->dropped[0].find("run_end_decode"), std::string::npos) << safe->dropped[0];
  ASSERT_EQ(safe->columns.size(), 1U);
  EXPECT_EQ(safe->columns[0].source_index, 1);

  auto batch = arrow::RecordBatch::Make(schema, 3, {ree, ids});
  auto safe_batch = mosaico::castToSchema(*batch, *safe);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  ASSERT_EQ((*safe_batch)->num_columns(), 1);
  EXPECT_TRUE((*safe_batch)->column(0)->Equals(*ids));
}

// PJ3 flattened and let the datastore skip what it could not plot. One exotic
// column must not cost the user every sibling curve, so an irreducible column is
// dropped with a diagnostic and the rest of the topic still imports.
TEST(IpcSafeSchema, IrreducibleColumnsAreDroppedAndSiblingsSurvive) {
  auto names = arrayOf<arrow::StringBuilder, std::string>({"a", "b", "c"});
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  arrow::Int8Builder type_builder;
  ASSERT_TRUE(type_builder.AppendValues({0, 0, 0}).ok());
  std::shared_ptr<arrow::Array> type_ids;
  ASSERT_TRUE(type_builder.Finish(&type_ids).ok());
  // A union whose child needs a rewrite: Arrow has no union-to-union cast.
  auto choice = *arrow::SparseUnionArray::Make(
      *type_ids, arrow::ArrayVector{castArray(names, arrow::utf8_view()), ids}, std::vector<std::string>{"s", "i"});

  auto schema = arrow::schema(
      {arrow::field("x", arrow::int64()), arrow::field("choice", choice->type()),
       arrow::field("label", arrow::utf8_view())});
  auto batch = arrow::RecordBatch::Make(schema, 3, {ids, choice, castArray(names, arrow::utf8_view())});

  auto safe = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe.ok()) << safe.status().ToString();
  ASSERT_EQ(safe->dropped.size(), 1U);
  // The diagnostic names the offending CHILD path, not just the union.
  EXPECT_NE(safe->dropped[0].find("'choice/s'"), std::string::npos) << safe->dropped[0];
  ASSERT_EQ(safe->columns.size(), 2U);
  EXPECT_EQ(safe->columns[0].source_index, 0) << "the siblings survive, renumbered";
  EXPECT_EQ(safe->columns[1].source_index, 2);
  ASSERT_EQ(safe->schema->num_fields(), 2);
  EXPECT_EQ(safe->schema->field(0)->name(), "x");
  EXPECT_EQ(safe->schema->field(1)->name(), "label");

  auto safe_batch = mosaico::castToSchema(*batch, *safe);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  auto bytes = mosaico::serializeIpcStream(**safe_batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(decoded->num_columns(), 2);
  EXPECT_TRUE(decoded->column(0)->Equals(*ids));
  EXPECT_TRUE(decoded->column(1)->Equals(*names));

  // A union whose children are all decodable is left alone, not dropped.
  auto plain_union = arrow::sparse_union({arrow::field("s", arrow::utf8()), arrow::field("i", arrow::int64())}, {0, 1});
  auto plain_schema = arrow::schema({arrow::field("choice", plain_union)});
  auto plain = mosaico::ipcSafeSchema(plain_schema);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  EXPECT_EQ(plain->schema, plain_schema);
  EXPECT_TRUE(plain->dropped.empty());
}

// A struct must not take its siblings down with it: PJ3 flattened first and lost
// only the leaf it could not plot, so the drop reaches INSIDE the struct and the
// array is reassembled around the survivors.
TEST(IpcSafeSchema, StructsLoseOnlyTheChildrenThatCannotBeFramed) {
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2, 3});
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, ids);
  auto names = arrayOf<arrow::StringBuilder, std::string>({"a", "b", "c"});
  auto hdr = *arrow::StructArray::Make(
      arrow::ArrayVector{castArray(names, arrow::utf8_view()), ree, ids},
      std::vector<std::string>{"frame_id", "weird", "seq"});
  auto schema = arrow::schema({arrow::field("hdr", hdr->type()), arrow::field("x", arrow::int64())});
  auto batch = arrow::RecordBatch::Make(schema, 3, {std::static_pointer_cast<arrow::Array>(hdr), ids});

  auto safe = mosaico::ipcSafeSchema(schema);
  ASSERT_TRUE(safe.ok()) << safe.status().ToString();
  ASSERT_EQ(safe->dropped.size(), 1U);
  EXPECT_NE(safe->dropped[0].find("'hdr/weird'"), std::string::npos) << safe->dropped[0];

  // hdr survives with two of three children, and the surviving view child is
  // still cast on the way out.
  ASSERT_EQ(safe->schema->num_fields(), 2);
  const auto& framed_hdr = safe->schema->field(0)->type();
  ASSERT_EQ(framed_hdr->num_fields(), 2);
  EXPECT_EQ(framed_hdr->field(0)->name(), "frame_id");
  EXPECT_TRUE(framed_hdr->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_EQ(framed_hdr->field(1)->name(), "seq");
  ASSERT_EQ(safe->columns.size(), 2U);
  ASSERT_EQ(safe->columns[0].children.size(), 2U);
  EXPECT_EQ(safe->columns[0].children[0].source_index, 0);
  EXPECT_EQ(safe->columns[0].children[1].source_index, 2) << "the REE child at index 1 is skipped";

  auto safe_batch = mosaico::castToSchema(*batch, *safe);
  ASSERT_TRUE(safe_batch.ok()) << safe_batch.status().ToString();
  auto bytes = mosaico::serializeIpcStream(**safe_batch);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  auto decoded = decodeSingleBatch(*bytes);
  ASSERT_NE(decoded, nullptr);
  auto decoded_hdr = std::static_pointer_cast<arrow::StructArray>(decoded->column(0));
  ASSERT_EQ(decoded_hdr->num_fields(), 2);
  EXPECT_TRUE(decoded_hdr->field(0)->Equals(*names)) << "values survive the reassembly";
  EXPECT_TRUE(decoded_hdr->field(1)->Equals(*ids));
  EXPECT_TRUE(decoded->column(1)->Equals(*ids));

  // A sliced batch is where a reassembly that mishandled offsets would show up:
  // the struct keeps the parent's offset while its children stay unsliced.
  auto sliced = mosaico::castToSchema(*batch->Slice(1, 2), *safe);
  ASSERT_TRUE(sliced.ok()) << sliced.status().ToString();
  auto sliced_bytes = mosaico::serializeIpcStream(**sliced);
  ASSERT_TRUE(sliced_bytes.ok()) << sliced_bytes.status().ToString();
  auto sliced_decoded = decodeSingleBatch(*sliced_bytes);
  ASSERT_NE(sliced_decoded, nullptr);
  ASSERT_EQ(sliced_decoded->num_rows(), 2);
  auto sliced_hdr = std::static_pointer_cast<arrow::StructArray>(sliced_decoded->column(0));
  EXPECT_TRUE(sliced_hdr->field(0)->Equals(*names->Slice(1, 2)));
  EXPECT_TRUE(sliced_hdr->field(1)->Equals(*ids->Slice(1, 2)));
}

// Nothing plottable left is the one case that still fails the topic outright.
TEST(IpcSafeSchema, FailsOnlyWhenNoColumnSurvives) {
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2, 3});
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, ids);
  // The struct's only leaf is undecodable, so the whole column goes and the
  // topic is left with nothing to plot.
  auto nested = arrow::schema({arrow::field("wrap", arrow::struct_({arrow::field("counter", ree->type())}))});

  auto status = mosaico::ipcSafeSchema(nested).status();
  EXPECT_TRUE(status.IsInvalid()) << status.ToString();
  EXPECT_NE(status.message().find("'wrap/counter'"), std::string::npos) << status.ToString();
}

}  // namespace
