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
