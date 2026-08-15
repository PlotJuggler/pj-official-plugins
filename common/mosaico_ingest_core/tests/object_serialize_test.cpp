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
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../arrow_ingest.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/occupancy_grid_codec.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"

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
      void* ctx, PJ_object_topic_handle_t topic, int64_t ts, const uint8_t* data, uint64_t size,
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
std::uint16_t readU16(const PJ::Span<const std::uint8_t>& data, std::size_t at) {
  std::uint16_t v = 0;
  std::memcpy(&v, data.data() + at, sizeof(std::uint16_t));
  return v;
}

// One list<float32> row (laser ranges, futures x/y/z attributes).
std::shared_ptr<arrow::Array> f32ListOneRow(const std::vector<float>& v) {
  arrow::FloatBuilder vb;
  EXPECT_TRUE(vb.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> values;
  EXPECT_TRUE(vb.Finish(&values).ok());
  auto offs = i32Col({0, static_cast<std::int32_t>(v.size())});
  auto r = arrow::ListArray::FromArrays(*offs, *values);
  EXPECT_TRUE(r.ok());
  return *r;
}
// One list<int8> row (occupancy_grid cell data).
std::shared_ptr<arrow::Array> i8ListOneRow(const std::vector<std::int8_t>& v) {
  arrow::Int8Builder vb;
  for (std::int8_t x : v) {
    EXPECT_TRUE(vb.Append(x).ok());
  }
  std::shared_ptr<arrow::Array> values;
  EXPECT_TRUE(vb.Finish(&values).ok());
  auto offs = i32Col({0, static_cast<std::int32_t>(v.size())});
  auto r = arrow::ListArray::FromArrays(*offs, *values);
  EXPECT_TRUE(r.ok());
  return *r;
}
// One list<uint16> row (futures reflectivity/beam_id attributes).
std::shared_ptr<arrow::Array> u16ListOneRow(const std::vector<std::uint16_t>& v) {
  arrow::UInt16Builder vb;
  for (std::uint16_t x : v) {
    EXPECT_TRUE(vb.Append(x).ok());
  }
  std::shared_ptr<arrow::Array> values;
  EXPECT_TRUE(vb.Finish(&values).ok());
  auto offs = i32Col({0, static_cast<std::int32_t>(v.size())});
  auto r = arrow::ListArray::FromArrays(*offs, *values);
  EXPECT_TRUE(r.ok());
  return *r;
}
// One list<struct{x,y,z double}> row (grid_cells `cells`).
std::shared_ptr<arrow::Array> xyzStructListOneRow(
    const std::vector<double>& xs, const std::vector<double>& ys, const std::vector<double>& zs) {
  auto items = structCol({"x", "y", "z"}, {f64Col(xs), f64Col(ys), f64Col(zs)});
  auto offs = i32Col({0, static_cast<std::int32_t>(xs.size())});
  auto r = arrow::ListArray::FromArrays(*offs, *items);
  EXPECT_TRUE(r.ok());
  return *r;
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

  auto pushed =
      mosaico::pushPointCloudRowsToHost({host, *ds, "/velodyne/points", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
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
  auto pushed =
      mosaico::pushPointCloudRowsToHost({host, *ds, "/velodyne/points", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
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

  auto pushed = mosaico::pushPoseRowsToHost({host, *ds, "/groundtruth/pose", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
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

  auto pushed = mosaico::pushPoseRowsToHost({host, *ds, "/odometry/odometry", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
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

// transform ontology (singular): one transform/row, top-level translation/
// rotation structs; parent = header.frame_id, child = target_frame_id.
TEST(ObjectSerialize, TransformSingularRoundTrips) {
  auto translation = structCol({"x", "y", "z"}, {f64Col({7.0}), f64Col({8.0}), f64Col({9.0})});
  auto rotation = structCol({"x", "y", "z", "w"}, {f64Col({0.0}), f64Col({0.0}), f64Col({0.0}), f64Col({1.0})});
  auto header = structCol({"frame_id"}, {strCol({"parent"})});
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("header", header->type()),
          arrow::field("translation", translation->type()),
          arrow::field("rotation", rotation->type()),
          arrow::field("target_frame_id", arrow::utf8()),
      }),
      {i64Col({111}), header, translation, rotation, strCol({"child"})});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushFrameTransformsRowsToHost({host, *ds, "/tf", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kFrameTransforms"})");

  auto ft = PJ::deserializeFrameTransforms(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(ft) << ft.error();
  ASSERT_EQ(ft->transforms.size(), 1u);
  EXPECT_EQ(ft->transforms[0].parent_frame_id, "parent");
  EXPECT_EQ(ft->transforms[0].child_frame_id, "child");
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.x, 7.0);
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.z, 9.0);
  EXPECT_DOUBLE_EQ(ft->transforms[0].rotation.w, 1.0);
  EXPECT_EQ(ft->transforms[0].timestamp, 111);
}

// frame_transform ontology: a `transforms` list<struct> per row → one batch.
TEST(ObjectSerialize, FrameTransformListRoundTrips) {
  auto translation = structCol({"x", "y", "z"}, {f64Col({1.0, 4.0}), f64Col({2.0, 5.0}), f64Col({3.0, 6.0})});
  auto rotation =
      structCol({"x", "y", "z", "w"}, {f64Col({0.0, 0.0}), f64Col({0.0, 0.0}), f64Col({0.0, 0.0}), f64Col({1.0, 1.0})});
  auto target = strCol({"base_link", "wheel"});
  auto header = structCol({"frame_id"}, {strCol({"odom", "base_link"})});
  auto items =
      structCol({"translation", "rotation", "target_frame_id", "header"}, {translation, rotation, target, header});
  auto offs = i32Col({0, 2});  // one row holding two transforms
  auto list_res = arrow::ListArray::FromArrays(*offs, *items);
  ASSERT_TRUE(list_res.ok());
  std::shared_ptr<arrow::Array> transforms_list = *list_res;

  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("transforms", transforms_list->type()),
      }),
      {i64Col({555}), transforms_list});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushFrameTransformsRowsToHost({host, *ds, "/tf", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.pushes.size(), 1u);

  auto ft = PJ::deserializeFrameTransforms(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(ft) << ft.error();
  ASSERT_EQ(ft->transforms.size(), 2u);
  EXPECT_EQ(ft->transforms[0].parent_frame_id, "odom");
  EXPECT_EQ(ft->transforms[0].child_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.x, 1.0);
  EXPECT_EQ(ft->transforms[1].parent_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(ft->transforms[1].translation.z, 6.0);
  EXPECT_EQ(ft->transforms[0].timestamp, 555);
  EXPECT_EQ(ft->transforms[1].timestamp, 555);
}

// occupancy_grid: info{resolution,width,height,origin} + dense data list<int8>.
TEST(ObjectSerialize, OccupancyGridRoundTrips) {
  auto origin = structCol(
      {"position", "orientation"},
      {structCol({"x", "y", "z"}, {f64Col({1.0}), f64Col({2.0}), f64Col({0.0})}),
       structCol({"x", "y", "z", "w"}, {f64Col({0.0}), f64Col({0.0}), f64Col({0.0}), f64Col({1.0})})});
  auto info =
      structCol({"resolution", "width", "height", "origin"}, {f64Col({0.05}), u32Col({2}), u32Col({2}), origin});
  auto header = structCol({"frame_id"}, {strCol({"map"})});
  auto data = i8ListOneRow({-1, 0, 50, 100});
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("info", info->type()),
          arrow::field("header", header->type()),
          arrow::field("data", arrow::list(arrow::int8())),
      }),
      {i64Col({7}), info, header, data});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushOccupancyGridRowsToHost({host, *ds, "/map", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kOccupancyGrid"})");

  auto grid = PJ::deserializeOccupancyGrid(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(grid) << grid.error();
  EXPECT_EQ(grid->width, 2u);
  EXPECT_EQ(grid->height, 2u);
  EXPECT_NEAR(grid->resolution, 0.05, 1e-6);
  EXPECT_EQ(grid->frame_id, "map");
  EXPECT_DOUBLE_EQ(grid->origin.position.x, 1.0);
  EXPECT_DOUBLE_EQ(grid->origin.position.y, 2.0);
  EXPECT_DOUBLE_EQ(grid->origin.orientation.w, 1.0);
  ASSERT_EQ(grid->data.size(), 4u);
  EXPECT_EQ(static_cast<std::int8_t>(grid->data.data()[0]), -1);  // unknown
  EXPECT_EQ(grid->data.data()[3], 100u);                          // occupied
  EXPECT_EQ(fake.pushes[0].ts_ns, 7);
}

// laser_scan: polar ranges expand to packed XYZ; non-finite returns dropped.
TEST(ObjectSerialize, LaserScanExpandsToPointCloud) {
  constexpr double kHalfPi = 1.57079632679489661923;
  auto header = structCol({"frame_id"}, {strCol({"laser"})});
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("header", header->type()),
          arrow::field("angle_min", arrow::float64()),
          arrow::field("angle_increment", arrow::float64()),
          arrow::field("range_min", arrow::float64()),
          arrow::field("range_max", arrow::float64()),
          arrow::field("ranges", arrow::list(arrow::float32())),
      }),
      {i64Col({33}), header, f64Col({0.0}), f64Col({kHalfPi}), f64Col({0.0}), f64Col({100.0}),
       f32ListOneRow({1.0F, 2.0F, std::numeric_limits<float>::infinity(), 3.0F})});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushLaserScanRowsToHost({host, *ds, "/scan", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kPointCloud"})");

  auto pc = PJ::deserializePointCloud(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(pc) << pc.error();
  EXPECT_EQ(pc->width, 3u);        // the infinite return is dropped
  EXPECT_EQ(pc->point_step, 12u);  // x,y,z float32 (no intensity column)
  ASSERT_EQ(pc->fields.size(), 3u);
  EXPECT_EQ(pc->fields[0].name, "x");
  EXPECT_EQ(pc->frame_id, "laser");
  EXPECT_NEAR(readF32(pc->data, 0), 1.0F, 1e-4F);        // beam0 (angle 0, r=1): x=1
  EXPECT_NEAR(readF32(pc->data, 4), 0.0F, 1e-4F);        // beam0: y=0
  EXPECT_NEAR(readF32(pc->data, 16), 2.0F, 1e-4F);       // beam1 (angle π/2, r=2): y=2 (point1 @12, y@+4)
  EXPECT_NEAR(readF32(pc->data, 24 + 4), -3.0F, 1e-4F);  // beam3 (angle 3π/2, r=3): y=-3 (point2 @24, y@+4)
}

// grid_cells: each cell → one flat CubePrimitive in a SceneEntities batch.
TEST(ObjectSerialize, GridCellsToSceneEntities) {
  auto header = structCol({"frame_id"}, {strCol({"map"})});
  auto cells = xyzStructListOneRow({1.0, 3.0}, {2.0, 4.0}, {0.0, 0.0});
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("header", header->type()),
          arrow::field("cell_width", arrow::float64()),
          arrow::field("cell_height", arrow::float64()),
          arrow::field("cells", cells->type()),
      }),
      {i64Col({222}), header, f64Col({0.1}), f64Col({0.2}), cells});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed = mosaico::pushGridCellsRowsToHost({host, *ds, "/grid_cells", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kSceneEntities"})");

  auto se = PJ::deserializeSceneEntities(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(se) << se.error();
  ASSERT_EQ(se->entities.size(), 1u);
  EXPECT_EQ(se->entities[0].frame_id, "map");
  ASSERT_EQ(se->entities[0].cubes.size(), 2u);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[0].pose.position.x, 1.0);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[0].pose.position.y, 2.0);
  EXPECT_NEAR(se->entities[0].cubes[0].size.x, 0.1, 1e-6);
  EXPECT_NEAR(se->entities[0].cubes[0].size.y, 0.2, 1e-6);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[1].pose.position.x, 3.0);
}

// futures `lidar`: columnar parallel list columns packed into one PointCloud.
TEST(ObjectSerialize, FuturesLidarColumnarPacksToPointCloud) {
  auto header = structCol({"frame_id"}, {strCol({"lidar"})});
  auto table = arrow::Table::Make(
      arrow::schema({
          arrow::field("timestamp_ns", arrow::int64()),
          arrow::field("header", header->type()),
          arrow::field("x", arrow::list(arrow::float32())),
          arrow::field("y", arrow::list(arrow::float32())),
          arrow::field("z", arrow::list(arrow::float32())),
          arrow::field("intensity", arrow::list(arrow::float32())),
          arrow::field("reflectivity", arrow::list(arrow::uint16())),
      }),
      {i64Col({88}), header, f32ListOneRow({1.0F, 2.0F}), f32ListOneRow({3.0F, 4.0F}), f32ListOneRow({5.0F, 6.0F}),
       f32ListOneRow({0.5F, 0.6F}), u16ListOneRow({10, 20})});

  FakeHost fake;
  auto host = fake.view();
  auto ds = host.createDataSource("seq");
  ASSERT_TRUE(ds) << ds.error();
  auto pushed =
      mosaico::pushColumnarPointCloudRowsToHost({host, *ds, "/lidar/points", "timestamp_ns", 0, 0, /*tee=*/{}}, table);
  ASSERT_TRUE(pushed) << pushed.error();
  EXPECT_EQ(pushed->pushed, 1);
  ASSERT_EQ(fake.object_topics.size(), 1u);
  EXPECT_EQ(fake.object_topics[0].metadata_json, R"({"builtin_object_type":"kPointCloud"})");

  auto pc = PJ::deserializePointCloud(fake.pushes[0].payload.data(), fake.pushes[0].payload.size());
  ASSERT_TRUE(pc) << pc.error();
  EXPECT_EQ(pc->width, 2u);
  EXPECT_EQ(pc->height, 1u);
  EXPECT_EQ(pc->frame_id, "lidar");
  // x@0,y@4,z@8,intensity@12 (f32), reflectivity@16 (u16) → point_step 18.
  ASSERT_EQ(pc->fields.size(), 5u);
  EXPECT_EQ(pc->fields[0].name, "x");
  EXPECT_EQ(pc->fields[3].name, "intensity");
  EXPECT_EQ(pc->fields[4].name, "reflectivity");
  EXPECT_EQ(pc->fields[4].datatype, PJ::sdk::PointField::Datatype::kUint16);
  EXPECT_EQ(pc->fields[4].offset, 16u);
  EXPECT_EQ(pc->point_step, 18u);
  ASSERT_EQ(pc->data.size(), 36u);               // 2 points * 18 bytes
  EXPECT_FLOAT_EQ(readF32(pc->data, 0), 1.0F);   // p0.x
  EXPECT_FLOAT_EQ(readF32(pc->data, 12), 0.5F);  // p0.intensity
  EXPECT_EQ(readU16(pc->data, 16), 10u);         // p0.reflectivity
  EXPECT_FLOAT_EQ(readF32(pc->data, 18), 2.0F);  // p1.x
  EXPECT_EQ(readU16(pc->data, 34), 20u);         // p1.reflectivity
}
