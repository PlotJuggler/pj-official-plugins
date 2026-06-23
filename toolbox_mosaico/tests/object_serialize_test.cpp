// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Locks in the per-row canonicalization contract for the Mosaico 3D-object
// ontologies, using the REAL server Arrow shapes (verified live against
// demo.mosaico.dev):
//   * point_cloud2 — ROS PointCloud2: a packed `data` blob + a `fields`
//     descriptor (list<struct{name,offset,datatype,count}>) + width/height/
//     point_step/row_step/... -> one sdk::PointCloud per row (copied through
//     verbatim, no packing, no decode).
//   * pose — top-level position/orientation structs -> sdk::PosesInFrame.
//   * motion_state (odometry) — the pose nested under a `pose` struct
//     (pose/position, pose/orientation) -> sdk::PosesInFrame.
//
// Each must register the topic ONCE with {"builtin_object_type":"kX"} (no
// codec-id key) and serialize via the pj_base canonical codec. The tests build
// synthetic tables matching those shapes and drive the real helpers against a
// recording fake toolbox host (no Flight/gRPC), then deserialize each pushed
// blob and assert the fields round-trip.

#include <arrow/api.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_nested.h>
#include <arrow/array/builder_primitive.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../src/arrow_ingest.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"

namespace {

// ---------------------------------------------------------------------------
// Recording fake host (mirror of image_serialize_test.cpp). Captures every
// registerObjectTopic + pushOwnedObject.
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

  static bool createDataSource(void* ctx, PJ_string_view_t, PJ_data_source_handle_t* out_source, PJ_error_t*)
      PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    out_source->id = h->next_id++;
    return true;
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
  v.register_object_topic = &FakeHost::registerObjectTopic;
  v.push_owned_object = &FakeHost::pushOwnedObject;
  return v;
}();

// ---- small Arrow array builders --------------------------------------------
std::shared_ptr<arrow::Array> i64Col(const std::vector<std::int64_t>& v) {
  arrow::Int64Builder b;
  EXPECT_TRUE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> i32Col(const std::vector<std::int32_t>& v) {
  arrow::Int32Builder b;
  EXPECT_TRUE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> u32Col(const std::vector<std::uint32_t>& v) {
  arrow::UInt32Builder b;
  EXPECT_TRUE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> f64Col(const std::vector<double>& v) {
  arrow::DoubleBuilder b;
  EXPECT_TRUE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> strCol(const std::vector<std::string>& v) {
  arrow::StringBuilder b;
  EXPECT_TRUE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> boolCol(const std::vector<bool>& v) {
  arrow::BooleanBuilder b;
  for (bool x : v) {
    EXPECT_TRUE(b.Append(x).ok());
  }
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> binCol(const std::vector<std::vector<std::uint8_t>>& rows) {
  arrow::BinaryBuilder b;
  for (const auto& r : rows) {
    EXPECT_TRUE(b.Append(r.data(), static_cast<std::int64_t>(r.size())).ok());
  }
  std::shared_ptr<arrow::Array> a;
  EXPECT_TRUE(b.Finish(&a).ok());
  return a;
}
std::shared_ptr<arrow::Array> structCol(
    const std::vector<std::string>& names, const std::vector<std::shared_ptr<arrow::Array>>& children) {
  auto r = arrow::StructArray::Make(children, names);
  EXPECT_TRUE(r.ok());
  return *r;
}
// One list<struct{name,offset,datatype,count}> row holding all the given channels.
std::shared_ptr<arrow::Array> fieldsOneRow(
    const std::vector<std::string>& names, const std::vector<std::uint32_t>& offsets,
    const std::vector<std::int64_t>& datatypes) {
  std::vector<std::uint32_t> counts(names.size(), 1U);
  auto items = structCol(
      {"name", "offset", "datatype", "count"}, {strCol(names), u32Col(offsets), i64Col(datatypes), u32Col(counts)});
  auto list_offsets = i32Col({0, static_cast<std::int32_t>(names.size())});  // single row [0, N)
  auto r = arrow::ListArray::FromArrays(*list_offsets, *items);
  EXPECT_TRUE(r.ok());
  return *r;
}

void appendF32(std::vector<std::uint8_t>& buf, float v) {
  std::uint8_t tmp[sizeof(float)];
  std::memcpy(tmp, &v, sizeof(float));
  buf.insert(buf.end(), tmp, tmp + sizeof(float));
}
float readF32(const PJ::Span<const std::uint8_t>& data, std::size_t at) {
  float v = 0.0F;
  std::memcpy(&v, data.data() + at, sizeof(float));
  return v;
}

}  // namespace

// point_cloud2: a 2-point cloud, point_step 16 (x,y,z,intensity f32), packed
// `data` + `fields` descriptor — the real ROS PointCloud2 shape.
TEST(ObjectSerialize, PointCloud2RoundTripsThroughCanonicalCodec) {
  std::vector<std::uint8_t> data;  // 2 points * 16 bytes
  appendF32(data, 1.0F);
  appendF32(data, 1.5F);
  appendF32(data, 1.25F);
  appendF32(data, 10.0F);
  appendF32(data, 2.0F);
  appendF32(data, 2.5F);
  appendF32(data, 2.25F);
  appendF32(data, 20.0F);

  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("frame_id", arrow::utf8()),
          arrow::field("width", arrow::uint32()),
          arrow::field("height", arrow::uint32()),
          arrow::field("point_step", arrow::uint32()),
          arrow::field("row_step", arrow::uint32()),
          arrow::field("is_bigendian", arrow::boolean()),
          arrow::field("is_dense", arrow::boolean()),
          arrow::field("data", arrow::binary()),
          arrow::field(
              "fields", arrow::list(
                            arrow::struct_(
                                {arrow::field("name", arrow::utf8()), arrow::field("offset", arrow::uint32()),
                                 arrow::field("datatype", arrow::int64()), arrow::field("count", arrow::uint32())}))),
      }),
      {
          i64Col({1000}),
          strCol({"velodyne"}),
          u32Col({2}),
          u32Col({1}),
          u32Col({16}),
          u32Col({32}),
          boolCol({false}),
          boolCol({true}),
          binCol({data}),
          fieldsOneRow({"x", "y", "z", "intensity"}, {0, 4, 8, 12}, {7, 7, 7, 7}),  // datatype 7 == FLOAT32
      });

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushPointCloudRowsToHost({host, *ds, "/velodyne/points", "timestamp_ns", 0, 0}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  EXPECT_EQ(pushed->skipped, 0);

  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kPointCloud"})");
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto pc = PJ::deserializePointCloud(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(pc) << pc.error();
  EXPECT_EQ(pc->width, 2u);
  EXPECT_EQ(pc->height, 1u);
  EXPECT_EQ(pc->point_step, 16u);
  EXPECT_EQ(pc->row_step, 32u);
  EXPECT_FALSE(pc->is_bigendian);
  EXPECT_TRUE(pc->is_dense);
  EXPECT_EQ(pc->frame_id, "velodyne");
  EXPECT_EQ(fake.pushes[0].ts_ns, 1000);
  ASSERT_EQ(pc->fields.size(), 4u);
  EXPECT_EQ(pc->fields[0].name, "x");
  EXPECT_EQ(pc->fields[0].datatype, PJ::sdk::PointField::Datatype::kFloat32);
  EXPECT_EQ(pc->fields[3].name, "intensity");
  EXPECT_EQ(pc->fields[3].offset, 12u);
  ASSERT_EQ(pc->data.size(), 32u);                // copied through verbatim
  EXPECT_FLOAT_EQ(readF32(pc->data, 0), 1.0F);    // point0.x
  EXPECT_FLOAT_EQ(readF32(pc->data, 12), 10.0F);  // point0.intensity
  EXPECT_FLOAT_EQ(readF32(pc->data, 16), 2.0F);   // point1.x
}

// Empty data / fields -> the row is skipped, not aborted.
TEST(ObjectSerialize, PointCloud2SkipsEmptyRow) {
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("data", arrow::binary()),
          arrow::field(
              "fields", arrow::list(
                            arrow::struct_(
                                {arrow::field("name", arrow::utf8()), arrow::field("offset", arrow::uint32()),
                                 arrow::field("datatype", arrow::int64()), arrow::field("count", arrow::uint32())}))),
      }),
      {
          i64Col({1000}),
          binCol({{}}),  // empty data
          fieldsOneRow({"x", "y", "z"}, {0, 4, 8}, {7, 7, 7}),
      });

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushPointCloudRowsToHost({host, *ds, "/velodyne/points", "timestamp_ns", 0, 0}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 0);
  EXPECT_EQ(pushed->skipped, 1);
  EXPECT_FALSE(pushed->first_error.empty());
}

// pose ontology: top-level position/orientation struct columns.
TEST(ObjectSerialize, PoseTopLevelRoundTripsThroughCanonicalCodec) {
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("frame_id", arrow::utf8()),
          arrow::field(
              "position", arrow::struct_(
                              {arrow::field("x", arrow::float64()), arrow::field("y", arrow::float64()),
                               arrow::field("z", arrow::float64())})),
          arrow::field(
              "orientation", arrow::struct_(
                                 {arrow::field("x", arrow::float64()), arrow::field("y", arrow::float64()),
                                  arrow::field("z", arrow::float64()), arrow::field("w", arrow::float64())})),
      }),
      {
          i64Col({9000}),
          strCol({"map"}),
          structCol({"x", "y", "z"}, {f64Col({5.0}), f64Col({6.0}), f64Col({7.0})}),
          structCol({"x", "y", "z", "w"}, {f64Col({0.0}), f64Col({0.0}), f64Col({0.0}), f64Col({1.0})}),
      });

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushPoseRowsToHost({host, *ds, "/groundtruth/pose", "timestamp_ns", 0, 0}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kPosesInFrame"})");
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto pf = PJ::deserializePosesInFrame(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(pf) << pf.error();
  EXPECT_EQ(pf->frame_id, "map");
  EXPECT_EQ(pf->timestamp_ns, 9000);
  ASSERT_EQ(pf->poses.size(), 1u);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.x, 5.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.z, 7.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].orientation.w, 1.0);
}

// motion_state (odometry): position/orientation nested under a `pose` struct.
TEST(ObjectSerialize, MotionStateNestedPoseRoundTrips) {
  auto position = structCol({"x", "y", "z"}, {f64Col({1.0}), f64Col({2.0}), f64Col({3.0})});
  auto orientation = structCol({"x", "y", "z", "w"}, {f64Col({0.0}), f64Col({0.0}), f64Col({0.0}), f64Col({1.0})});
  auto pose = structCol({"position", "orientation"}, {position, orientation});

  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("frame_id", arrow::utf8()),
          arrow::field("pose", pose->type()),
      }),
      {i64Col({4242}), strCol({"odom"}), pose});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();

  auto pushed = mosaico::pushPoseRowsToHost({host, *ds, "/odometry/odometry", "timestamp_ns", 0, 0}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto pf = PJ::deserializePosesInFrame(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(pf) << pf.error();
  EXPECT_EQ(pf->frame_id, "odom");
  ASSERT_EQ(pf->poses.size(), 1u);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.x, 1.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.z, 3.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].orientation.w, 1.0);
}
