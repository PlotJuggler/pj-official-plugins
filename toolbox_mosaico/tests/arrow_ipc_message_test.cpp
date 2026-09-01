// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// arrow-ipc framing: timestamp-field detection, the message host timestamp,
// and the one-batch-per-stream round trip `parser_arrow` decodes.

#include "../src/arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/dictionary.h>
#include <arrow/ipc/reader.h>
#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

std::shared_ptr<arrow::Schema> schemaOf(const std::vector<std::shared_ptr<arrow::Field>>& fields) {
  return arrow::schema(fields);
}

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
  auto schema = schemaOf({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  return arrow::RecordBatch::Make(
      schema, 3,
      {arrayOf<arrow::Int64Builder, std::int64_t>({1000, 1001, 1002}),
       arrayOf<arrow::DoubleBuilder, double>({0.0, 1.0, 2.0})});
}

TEST(DetectTimestampField, ArrowTimestampTypeWins) {
  auto schema = schemaOf(
      {arrow::field("timestamp_ns", arrow::int64()), arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MICRO))});
  EXPECT_EQ(mosaico::detectTimestampField(*schema), "stamp");
}

TEST(DetectTimestampField, NamePriorityMostSpecificFirst) {
  auto schema = schemaOf(
      {arrow::field("ts", arrow::int64()), arrow::field("time", arrow::int64()),
       arrow::field("recording_timestamp_ns", arrow::int64())});
  EXPECT_EQ(mosaico::detectTimestampField(*schema), "recording_timestamp_ns");
  EXPECT_EQ(mosaico::detectTimestampField(*schemaOf({arrow::field("time", arrow::int64())})), "time");
  EXPECT_EQ(mosaico::detectTimestampField(*schemaOf({arrow::field("ts", arrow::int64())})), "ts");
}

TEST(DetectTimestampField, EmptyWhenNothingMatches) {
  EXPECT_EQ(mosaico::detectTimestampField(*schemaOf({arrow::field("x", arrow::float64())})), "");
}

TEST(FirstRowTimestampNs, IntegerIsNanoseconds) {
  EXPECT_EQ(mosaico::firstRowTimestampNs(*scalarBatch(), "timestamp_ns"), 1000);
}

TEST(FirstRowTimestampNs, DoubleIsSeconds) {
  auto schema = schemaOf({arrow::field("t", arrow::float64())});
  auto batch = arrow::RecordBatch::Make(schema, 1, {arrayOf<arrow::DoubleBuilder, double>({1.5})});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*batch, "t"), 1'500'000'000LL);
}

TEST(FirstRowTimestampNs, TimestampScaledByUnit) {
  auto schema = schemaOf({arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MILLI))});
  arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MILLI), arrow::default_memory_pool());
  ASSERT_TRUE(builder.Append(7).ok());
  std::shared_ptr<arrow::Array> array;
  ASSERT_TRUE(builder.Finish(&array).ok());
  auto batch = arrow::RecordBatch::Make(schema, 1, {array});
  EXPECT_EQ(mosaico::firstRowTimestampNs(*batch, "stamp"), 7'000'000LL);
}

TEST(FirstRowTimestampNs, EmptyOnMissingFieldEmptyBatchOrEmptyName) {
  auto batch = scalarBatch();
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, "nope").has_value());
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch, "").has_value());
  EXPECT_FALSE(mosaico::firstRowTimestampNs(*batch->Slice(0, 0), "timestamp_ns").has_value());
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

TEST(SerializeIpcSchema, DecodesToTheSameSchema) {
  auto batch = scalarBatch();
  auto bytes = mosaico::serializeIpcSchema(*batch->schema());
  ASSERT_TRUE(bytes.ok());
  arrow::io::BufferReader source(*bytes);
  arrow::ipc::DictionaryMemo memo;
  auto schema = arrow::ipc::ReadSchema(&source, &memo);
  ASSERT_TRUE(schema.ok()) << schema.status().ToString();
  EXPECT_TRUE((*schema)->Equals(*batch->schema()));
}

TEST(ParserConfigJson, CarriesBothKeys) {
  auto config = nlohmann::json::parse(mosaico::parserConfigJson("timestamp_ns", 33));
  EXPECT_EQ(config.at("timestamp_column"), "timestamp_ns");
  EXPECT_EQ(config.at("synthetic_interval_ns"), 33);
  EXPECT_EQ(nlohmann::json::parse(mosaico::parserConfigJson("", 0)).at("timestamp_column"), "");
}

}  // namespace
