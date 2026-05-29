// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Locks in the per-frame image serialization contract for a Mosaico image
// topic. The real server ships image topics as Arrow record batches whose
// `data`/`encoding`/`format` columns are the Arrow `*_view` variants
// (BINARY_VIEW / STRING_VIEW) and whose geometry (width/height/stride/
// encoding/is_bigendian) is PER ROW, not topic-level.
//
// pushImageRowsToHost must:
//   * register the object topic ONCE with the canonical metadata JSON
//     {"builtin_object_type":"kImage","image_codec":"pj_image_v1"};
//   * serialize ONE PJ::sdk::Image PER ROW with that row's own geometry,
//     using the pj_base canonical codec (PJ::serializeImage);
//   * read STRING_VIEW / BINARY_VIEW columns (the suspected "barely
//     downloads" root cause — old BINARY/STRING readers returned empty);
//   * fall back encoding <- format when the per-row encoding is empty
//     (pure-compressed topics carry only `format`);
//   * skip rows missing required columns, recording ONE error, without
//     aborting the whole topic.
//
// These tests build synthetic in-memory tables using the *_view types and
// drive the real helper against a recording fake toolbox host (no Flight/gRPC).

#include <arrow/api.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../src/arrow_ingest.hpp"
#include "pj_base/builtin/image_codec.hpp"

namespace {

// ---------------------------------------------------------------------------
// Recording fake host (mirrors tests/fetch_grouping_test.cpp). Captures every
// registerObjectTopic + pushOwnedObject so the test can assert the topic
// metadata and inspect each pushed blob.
// ---------------------------------------------------------------------------
struct RecordedObjectTopic {
  std::uint32_t source_id = 0;
  std::string name;
  std::string metadata_json;
  std::uint32_t id = 0;
};
struct RecordedPush {
  std::uint32_t topic_id = 0;
  std::int64_t ts_ns = 0;
  std::vector<std::uint8_t> payload;
};

struct FakeHost {
  std::vector<std::string> data_sources;
  std::vector<RecordedObjectTopic> object_topics;
  std::vector<RecordedPush> pushes;
  std::uint32_t next_id = 1;
  std::mutex mu;

  PJ::sdk::ToolboxHostView view() {
    PJ_toolbox_host_t host{};
    host.ctx = this;
    host.vtable = &kVtable;
    return PJ::sdk::ToolboxHostView(host);
  }

  static FakeHost* self(void* ctx) {
    return static_cast<FakeHost*>(ctx);
  }
  static std::string toStr(PJ_string_view_t s) {
    return (s.data != nullptr && s.size > 0) ? std::string(s.data, s.size) : std::string();
  }

  static bool createDataSource(void* ctx, PJ_string_view_t name, PJ_data_source_handle_t* out_source, PJ_error_t*)
      PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    const std::uint32_t id = h->next_id++;
    h->data_sources.push_back(toStr(name));
    out_source->id = id;
    return true;
  }
  static bool ensureTopic(void*, PJ_data_source_handle_t, PJ_string_view_t, PJ_topic_handle_t*, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool ensureField(
      void*, PJ_topic_handle_t, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t*, PJ_error_t*) PJ_NOEXCEPT {
    return false;
  }
  static bool appendRecord(void*, PJ_topic_handle_t, int64_t, const PJ_named_field_value_t*, size_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool appendBoundRecord(void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, size_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool appendArrowStream(void*, PJ_topic_handle_t, struct ArrowArrayStream*, PJ_string_view_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool acquireCatalogSnapshot(void*, PJ_catalog_snapshot_t*, PJ_error_t*) PJ_NOEXCEPT {
    return false;
  }
  static bool readSeriesArrow(void*, PJ_field_handle_t, struct ArrowSchema*, struct ArrowArray*, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool registerObjectTopic(
      void* ctx, PJ_data_source_handle_t source, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
      PJ_object_topic_handle_t* out_handle, PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    const std::uint32_t id = h->next_id++;
    h->object_topics.push_back({source.id, toStr(topic_name), toStr(metadata_json), id});
    out_handle->id = id;
    return true;
  }
  static bool pushOwnedObject(
      void* ctx, PJ_object_topic_handle_t topic, int64_t ts, const uint8_t* data, size_t size,
      PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    RecordedPush rec;
    rec.topic_id = topic.id;
    rec.ts_ns = ts;
    if (data != nullptr && size > 0) {
      rec.payload.assign(data, data + size);
    }
    h->pushes.push_back(std::move(rec));
    return true;
  }

  static const PJ_toolbox_host_vtable_t kVtable;
};

const PJ_toolbox_host_vtable_t FakeHost::kVtable = [] {
  PJ_toolbox_host_vtable_t v{};
  v.abi_version = 0;
  v.struct_size = sizeof(PJ_toolbox_host_vtable_t);
  v.create_data_source = &FakeHost::createDataSource;
  v.ensure_topic = &FakeHost::ensureTopic;
  v.ensure_field = &FakeHost::ensureField;
  v.append_record = &FakeHost::appendRecord;
  v.append_bound_record = &FakeHost::appendBoundRecord;
  v.append_arrow_stream = &FakeHost::appendArrowStream;
  v.acquire_catalog_snapshot = &FakeHost::acquireCatalogSnapshot;
  v.read_series_arrow = &FakeHost::readSeriesArrow;
  v.register_object_topic = &FakeHost::registerObjectTopic;
  v.push_owned_object = &FakeHost::pushOwnedObject;
  return v;
}();

// The exact canonical metadata contract the PJ4 consumer keys on.
constexpr const char* kCanonicalImageMeta = R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})";

// One synthetic image row's worth of column values.
struct RowSpec {
  std::optional<std::vector<std::uint8_t>> data;  // nullopt -> null (missing data)
  std::string format;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t stride = 0;
  std::string encoding;
  std::optional<bool> is_bigendian;  // nullopt -> null
  std::int64_t timestamp_ns = 0;
};

// Build an Arrow Table mimicking the real server image schema: data is
// BINARY_VIEW, encoding/format are STRING_VIEW, geometry int32, is_bigendian
// bool, timestamp_ns int64. Per-row values come from `rows`.
std::shared_ptr<arrow::Table> makeImageViewTable(const std::vector<RowSpec>& rows) {
  arrow::Int64Builder ts_b;
  arrow::BinaryViewBuilder data_b;
  arrow::StringViewBuilder format_b;
  arrow::StringViewBuilder encoding_b;
  arrow::Int32Builder width_b;
  arrow::Int32Builder height_b;
  arrow::Int32Builder stride_b;
  arrow::BooleanBuilder bigendian_b;

  for (const auto& r : rows) {
    EXPECT_TRUE(ts_b.Append(r.timestamp_ns).ok());
    if (r.data.has_value()) {
      EXPECT_TRUE(data_b.Append(r.data->data(), static_cast<int64_t>(r.data->size())).ok());
    } else {
      EXPECT_TRUE(data_b.AppendNull().ok());
    }
    EXPECT_TRUE(format_b.Append(r.format).ok());
    EXPECT_TRUE(encoding_b.Append(r.encoding).ok());
    EXPECT_TRUE(width_b.Append(r.width).ok());
    EXPECT_TRUE(height_b.Append(r.height).ok());
    EXPECT_TRUE(stride_b.Append(r.stride).ok());
    if (r.is_bigendian.has_value()) {
      EXPECT_TRUE(bigendian_b.Append(*r.is_bigendian).ok());
    } else {
      EXPECT_TRUE(bigendian_b.AppendNull().ok());
    }
  }

  std::shared_ptr<arrow::Array> ts_a;
  std::shared_ptr<arrow::Array> data_a;
  std::shared_ptr<arrow::Array> format_a;
  std::shared_ptr<arrow::Array> encoding_a;
  std::shared_ptr<arrow::Array> width_a;
  std::shared_ptr<arrow::Array> height_a;
  std::shared_ptr<arrow::Array> stride_a;
  std::shared_ptr<arrow::Array> bigendian_a;
  EXPECT_TRUE(ts_b.Finish(&ts_a).ok());
  EXPECT_TRUE(data_b.Finish(&data_a).ok());
  EXPECT_TRUE(format_b.Finish(&format_a).ok());
  EXPECT_TRUE(encoding_b.Finish(&encoding_a).ok());
  EXPECT_TRUE(width_b.Finish(&width_a).ok());
  EXPECT_TRUE(height_b.Finish(&height_a).ok());
  EXPECT_TRUE(stride_b.Finish(&stride_a).ok());
  EXPECT_TRUE(bigendian_b.Finish(&bigendian_a).ok());

  auto schema = arrow::schema({
      arrow::field("timestamp_ns", arrow::int64()),
      arrow::field("data", arrow::binary_view()),
      arrow::field("format", arrow::utf8_view()),
      arrow::field("encoding", arrow::utf8_view()),
      arrow::field("width", arrow::int32()),
      arrow::field("height", arrow::int32()),
      arrow::field("stride", arrow::int32()),
      arrow::field("is_bigendian", arrow::boolean()),
  });
  return arrow::Table::Make(schema, {ts_a, data_a, format_a, encoding_a, width_a, height_a, stride_a, bigendian_a});
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> vs) {
  std::vector<std::uint8_t> out;
  out.reserve(vs.size());
  for (int v : vs) {
    out.push_back(static_cast<std::uint8_t>(v));
  }
  return out;
}

}  // namespace

// Two rows with DIFFERENT geometry prove per-frame (not row-0) serialization,
// and the *_view column types prove BINARY_VIEW/STRING_VIEW handling.
TEST(ImageSerialize, PerRowGeometryRoundTripsThroughCanonicalCodec) {
  // row0: rgb8 2x2 stride 6 (12 bytes); row1: rgb8 3x1 stride 9 (9 bytes).
  std::vector<RowSpec> rows;
  rows.push_back(RowSpec{bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}), "png", 2, 2, 6, "rgb8", false, 1000});
  rows.push_back(RowSpec{bytes({21, 22, 23, 24, 25, 26, 27, 28, 29}), "png", 3, 1, 9, "rgb8", false, 2000});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/camera/jai/rgb/image", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 2);
  EXPECT_EQ(pushed->skipped, 0);

  // Registered exactly once with the canonical metadata.
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].name, "/camera/jai/rgb/image");
  EXPECT_EQ(fake.object_topics[0].metadata_json, kCanonicalImageMeta);

  // Two blobs pushed, each round-tripping to its OWN row geometry.
  ASSERT_EQ(fake.pushes.size(), 2u);

  auto img0 = PJ::deserializeImage(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(img0) << img0.error();
  EXPECT_EQ(img0->width, 2u);
  EXPECT_EQ(img0->height, 2u);
  EXPECT_EQ(img0->row_step, 6u);
  EXPECT_EQ(img0->encoding, "rgb8");
  EXPECT_FALSE(img0->is_bigendian);
  EXPECT_EQ(fake.pushes[0].ts_ns, 1000);
  ASSERT_EQ(img0->data.size(), 12u);
  EXPECT_EQ(img0->data[0], 1);
  EXPECT_EQ(img0->data[11], 12);

  auto img1 = PJ::deserializeImage(fake.pushes[1].payload.data(), fake.pushes[1].payload.size());
  ASSERT_TRUE(img1) << img1.error();
  EXPECT_EQ(img1->width, 3u);
  EXPECT_EQ(img1->height, 1u);
  EXPECT_EQ(img1->row_step, 9u);
  EXPECT_EQ(img1->encoding, "rgb8");
  EXPECT_EQ(fake.pushes[1].ts_ns, 2000);
  ASSERT_EQ(img1->data.size(), 9u);
  EXPECT_EQ(img1->data[0], 21);
  EXPECT_EQ(img1->data[8], 29);
}

// encoding empty, format="jpeg" -> deserialized encoding == "jpeg".
TEST(ImageSerialize, FallsBackToFormatWhenEncodingEmpty) {
  std::vector<RowSpec> rows;
  // A pure-compressed row: no width/height (0), only `format`. encoding empty.
  rows.push_back(RowSpec{bytes({0xFF, 0xD8, 0xFF, 0xE0}), "jpeg", 0, 0, 0, "", false, 5000});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/cam/compressed", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto img = PJ::deserializeImage(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(img) << img.error();
  EXPECT_EQ(img->encoding, "jpeg");
  ASSERT_EQ(img->data.size(), 4u);
  EXPECT_EQ(img->data[0], 0xFF);
}

// mono16 with is_bigendian=true must round-trip true.
TEST(ImageSerialize, BigEndianRoundTrips) {
  std::vector<RowSpec> rows;
  rows.push_back(RowSpec{bytes({0, 1, 2, 3, 4, 5, 6, 7}), "", 2, 2, 4, "mono16", true, 7000});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/cam/depth", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto img = PJ::deserializeImage(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(img) << img.error();
  EXPECT_EQ(img->encoding, "mono16");
  EXPECT_TRUE(img->is_bigendian);
}

// [M10] A NULL is_bigendian column must default to the HOST's native
// endianness (per the canonical Mosaico Image model), NOT a hard `false`.
// mono16 carries a null is_bigendian here; the deserialized blob must reflect
// the test host's byte order.
TEST(ImageSerialize, NullBigEndianDefaultsToHostEndianness) {
  std::vector<RowSpec> rows;
  // is_bigendian = nullopt -> null in the Arrow column.
  rows.push_back(RowSpec{bytes({0, 1, 2, 3, 4, 5, 6, 7}), "", 2, 2, 4, "mono16", std::nullopt, 9000});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/cam/depth", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto img = PJ::deserializeImage(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(img) << img.error();
  constexpr bool kHostIsBigEndian = (std::endian::native == std::endian::big);
  EXPECT_EQ(img->is_bigendian, kHostIsBigEndian);
}

// [M11] A row whose `encoding` arrives EMPTY but carries a `format` is a
// pure-compressed frame: encoding falls back to `format`, and because the
// per-row encoding was empty the row is treated as compressed — so the
// non-positive-geometry gate is bypassed and the row is pushed (not skipped),
// even with width/height/stride all 0. This exercises the `encoding_was_empty`
// capture (empty BEFORE the format fallback overwrites encoding).
TEST(ImageSerialize, EmptyEncodingWithFormatIsCompressedAndKeepsZeroGeometry) {
  std::vector<RowSpec> rows;
  // encoding="" + format="png", geometry 0/0/0 — a raw frame with this geometry
  // would be rejected; a compressed one must pass.
  rows.push_back(RowSpec{bytes({0x89, 0x50, 0x4E, 0x47}), "png", 0, 0, 0, "", false, 4200});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/cam/png", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);  // compressed -> NOT skipped despite 0 geometry.
  EXPECT_EQ(pushed->skipped, 0);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto img = PJ::deserializeImage(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(img) << img.error();
  EXPECT_EQ(img->encoding, "png");  // fell back from `format`.
}

// A row missing `data` is skipped; others still push; one error recorded.
TEST(ImageSerialize, MissingDataRowSkippedOthersPushed) {
  std::vector<RowSpec> rows;
  rows.push_back(RowSpec{bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}), "png", 2, 2, 6, "rgb8", false, 1000});
  rows.push_back(RowSpec{std::nullopt, "png", 2, 2, 6, "rgb8", false, 2000});  // null data
  rows.push_back(RowSpec{bytes({21, 22, 23, 24, 25, 26, 27, 28, 29}), "png", 3, 1, 9, "rgb8", false, 3000});
  auto table = makeImageViewTable(rows);

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushImageRowsToHost(host, *ds, "/cam/raw", table, "timestamp_ns", 0, 0);
  ASSERT_TRUE(pushed) << pushed.error();
  // The two good rows pushed; the null-data row skipped.
  EXPECT_EQ(pushed->pushed, 2);
  ASSERT_EQ(fake.pushes.size(), 2u);
  EXPECT_EQ(fake.pushes[0].ts_ns, 1000);
  EXPECT_EQ(fake.pushes[1].ts_ns, 3000);

  // One skip error recorded.
  EXPECT_EQ(pushed->skipped, 1);
  EXPECT_FALSE(pushed->first_error.empty());
}
