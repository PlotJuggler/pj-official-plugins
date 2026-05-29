// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for arrow_ingest::detectTimestampColumn. These touch only
// the ArrowSchema C ABI struct so they don't link Mosaico Flight, gRPC,
// or the toolbox host — they can run in any plugin's test target.

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/arrow_ingest.hpp"

namespace {

// Helper: build an ArrowSchema by exporting an arrow::Schema via the
// Arrow C++ bridge. Caller owns the resulting struct and must call
// release() on it.
struct ManagedSchema {
  ArrowSchema schema{};
  ~ManagedSchema() {
    if (schema.release) {
      schema.release(&schema);
    }
  }
};

ManagedSchema makeSchema(const std::vector<std::shared_ptr<arrow::Field>>& fields) {
  ManagedSchema out;
  auto schema = arrow::schema(fields);
  auto status = arrow::ExportSchema(*schema, &out.schema);
  EXPECT_TRUE(status.ok()) << status.ToString();
  return out;
}

}  // namespace

TEST(DetectTimestampColumnTest, NullSchemaReturnsEmpty) {
  EXPECT_EQ(mosaico::detectTimestampColumn(nullptr), "");
}

TEST(DetectTimestampColumnTest, ArrowTimestampTypeWins) {
  // Field order is not "timestamp_ns" first, but the Arrow TIMESTAMP
  // pass takes precedence over name heuristics.
  auto guard = makeSchema({
      arrow::field("recording_timestamp_ns", arrow::int64()),
      arrow::field("captured_at", arrow::timestamp(arrow::TimeUnit::NANO)),
      arrow::field("value", arrow::float64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "captured_at");
}

TEST(DetectTimestampColumnTest, FallsBackToTimestampNs) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("timestamp_ns", arrow::int64()),
      arrow::field("time", arrow::int64()),  // less specific — must not win
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "timestamp_ns");
}

TEST(DetectTimestampColumnTest, FallsBackToRecordingTimestampNs) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("recording_timestamp_ns", arrow::int64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "recording_timestamp_ns");
}

TEST(DetectTimestampColumnTest, FallsBackToPlainTimestamp) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("timestamp", arrow::int64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "timestamp");
}

TEST(DetectTimestampColumnTest, FallsBackToTime) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("time", arrow::int64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "time");
}

TEST(DetectTimestampColumnTest, FallsBackToTs) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("ts", arrow::int64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "ts");
}

TEST(DetectTimestampColumnTest, EmptyIfNoMatch) {
  auto guard = makeSchema({
      arrow::field("value", arrow::float64()),
      arrow::field("name", arrow::utf8()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "");
}

TEST(DetectTimestampColumnTest, MostSpecificNameWinsAmongHeuristics) {
  // When both "timestamp" and "timestamp_ns" exist, "_ns" form should
  // win because it's checked first in the heuristic list.
  auto guard = makeSchema({
      arrow::field("timestamp", arrow::int64()),
      arrow::field("timestamp_ns", arrow::int64()),
  });
  EXPECT_EQ(mosaico::detectTimestampColumn(&guard.schema), "timestamp_ns");
}

// A Utf8View column anywhere in the batch makes pj_datastore's import null the
// whole record batch; normalizeViewColumns casts it back to canonical Utf8 so
// the scalar pipeline ingests cleanly. Doubles must survive untouched.
TEST(NormalizeViewColumns, CastsUtf8ViewToUtf8PreservingValues) {
  arrow::StringViewBuilder sv;
  ASSERT_TRUE(sv.Append("alpha").ok());
  ASSERT_TRUE(sv.Append("beta").ok());
  std::shared_ptr<arrow::Array> sv_arr;
  ASSERT_TRUE(sv.Finish(&sv_arr).ok());

  arrow::DoubleBuilder db;
  ASSERT_TRUE(db.Append(1.5).ok());
  ASSERT_TRUE(db.Append(2.5).ok());
  std::shared_ptr<arrow::Array> d_arr;
  ASSERT_TRUE(db.Finish(&d_arr).ok());

  auto table = arrow::Table::Make(
      arrow::schema({arrow::field("frame_id", arrow::utf8_view()), arrow::field("value", arrow::float64())}),
      {sv_arr, d_arr});
  ASSERT_EQ(table->column(0)->type()->id(), arrow::Type::STRING_VIEW);

  auto result = mosaico::normalizeViewColumns(table);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  auto out = result.ValueOrDie();

  EXPECT_EQ(out->schema()->field(0)->type()->id(), arrow::Type::STRING);
  EXPECT_EQ(out->schema()->field(1)->type()->id(), arrow::Type::DOUBLE);
  ASSERT_EQ(out->num_rows(), 2);
  auto strs = std::static_pointer_cast<arrow::StringArray>(out->column(0)->chunk(0));
  EXPECT_EQ(strs->GetString(0), "alpha");
  EXPECT_EQ(strs->GetString(1), "beta");
  auto dbls = std::static_pointer_cast<arrow::DoubleArray>(out->column(1)->chunk(0));
  EXPECT_DOUBLE_EQ(dbls->Value(0), 1.5);
  EXPECT_DOUBLE_EQ(dbls->Value(1), 2.5);
}

TEST(NormalizeViewColumns, PassesThroughWhenNoViewTypes) {
  arrow::DoubleBuilder db;
  ASSERT_TRUE(db.Append(1.0).ok());
  std::shared_ptr<arrow::Array> d_arr;
  ASSERT_TRUE(db.Finish(&d_arr).ok());
  auto table = arrow::Table::Make(arrow::schema({arrow::field("v", arrow::float64())}), {d_arr});
  auto result = mosaico::normalizeViewColumns(table);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.ValueOrDie(), table);  // unchanged → same pointer
}
