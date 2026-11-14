#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "builtin_object_compat.hpp"
#include "navmap_cdr_reader.hpp"
#include "navmap_mesh.hpp"
#include "navmap_msg_decoder.hpp"
#include "pj_base/builtin/mesh_map.hpp"
#include "pj_base/builtin/mesh_map_update.hpp"
#include "pj_base/parser_route_claims_protocol.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_runtime_recorder.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_NAVMAP_PARSER_PLUGIN_PATH
#error "PJ_NAVMAP_PARSER_PLUGIN_PATH must be defined"
#endif
#ifndef PJ_NAVMAP_TEST_DATA_DIR
#error "PJ_NAVMAP_TEST_DATA_DIR must be defined"
#endif

namespace {

constexpr std::string_view kNavMapType = "navmap_ros_interfaces/msg/NavMap";
constexpr std::string_view kNavMapLayerType = "navmap_ros_interfaces/msg/NavMapLayer";
constexpr int64_t kReceiveTime = 5'000'000'000;

// The bundle an MCAP recorded with the current navmap_ros_interfaces carries.
constexpr std::string_view kCurrentSchema = R"(# Compact NavMap transport.
std_msgs/Header header
float32[] positions_x
float32[] positions_y
float32[] positions_z
bool has_vertex_rgba
uint8[] colors_r
uint8[] colors_g
uint8[] colors_b
uint8[] colors_a
uint32[] navcels_v0
uint32[] navcels_v1
uint32[] navcels_v2
navmap_ros_interfaces/NavMapSurface[] surfaces
navmap_ros_interfaces/NavMapLayer[] layers
uint64 geometry_id
navmap_ros_interfaces/NavMapLink[] links
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: navmap_ros_interfaces/NavMapSurface
# A surface groups a subset of triangles in a frame.
string frame_id
uint32[] navcels
================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
std_msgs/Header header
uint8 U8 = 0
uint8 F32 = 1
uint8 F64 = 2
uint8 I16 = 3
string name
uint8 type         # 0=U8, 1=F32, 2=F64, 3=I16
string description
string unit
bool has_unknown
float64 unknown_value
uint64 geometry_id
uint32[] indices
uint8[]   data_u8
float32[] data_f32
float64[] data_f64
int16[]   data_i16
================================================================================
MSG: navmap_ros_interfaces/NavMapLink
uint8 GENERIC = 0
uint8 STAIRS = 2
uint32 from_navcel
uint32 to_navcel
uint8 type
float32 length        # travel length (m)
bool bidirectional
)";

// The first released layout (file format version 1): no metadata, no int16, no
// geometry_id, no links.
constexpr std::string_view kV1Schema = R"(std_msgs/Header header
float32[] positions_x
float32[] positions_y
float32[] positions_z
bool has_vertex_rgba
uint8[] colors_r
uint8[] colors_g
uint8[] colors_b
uint8[] colors_a
uint32[] navcels_v0
uint32[] navcels_v1
uint32[] navcels_v2
navmap_ros_interfaces/NavMapSurface[] surfaces
navmap_ros_interfaces/NavMapLayer[] layers
================================================================================
MSG: navmap_ros_interfaces/NavMapSurface
string frame_id
uint32[] navcels
================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
std_msgs/Header header
uint8 U8 = 0
string name
uint8 type
uint8[]   data_u8
float32[] data_f32
float64[] data_f64
)";

// Little-endian XCDR1 writer; alignment is relative to the encapsulation header.
class CdrWriter {
 public:
  CdrWriter() : buf_{0x00, 0x01, 0x00, 0x00} {}

  template <typename T>
  void put(T value) {
    align(sizeof(T));
    const auto offset = buf_.size();
    buf_.resize(offset + sizeof(T));
    std::memcpy(buf_.data() + offset, &value, sizeof(T));
  }
  void putString(std::string_view s) {
    put<uint32_t>(static_cast<uint32_t>(s.size() + 1));
    buf_.insert(buf_.end(), s.begin(), s.end());
    buf_.push_back(0);
  }
  template <typename T>
  void putSeq(const std::vector<T>& values) {
    put<uint32_t>(static_cast<uint32_t>(values.size()));
    for (const T v : values) {
      put<T>(v);
    }
  }
  void putHeader(int32_t sec, uint32_t nanosec, std::string_view frame) {
    put<int32_t>(sec);
    put<uint32_t>(nanosec);
    putString(frame);
  }
  [[nodiscard]] const std::vector<uint8_t>& bytes() const {
    return buf_;
  }

 private:
  void align(size_t n) {
    while ((buf_.size() - 4) % n != 0) {
      buf_.push_back(0);
    }
  }
  std::vector<uint8_t> buf_;
};

struct TestLayer {
  std::string name;
  uint8_t type = 1;
  std::string unit;
  bool has_unknown = false;
  double unknown_value = 0.0;
  std::vector<uint32_t> indices;
  std::vector<uint8_t> u8;
  std::vector<float> f32;
  std::vector<double> f64;
  std::vector<int16_t> i16;
  int32_t sec = 0;
  uint32_t nanosec = 0;
};

TestLayer f32Layer(std::string name, std::vector<float> values) {
  TestLayer layer;
  layer.name = std::move(name);
  layer.type = 1;
  layer.f32 = std::move(values);
  return layer;
}

TestLayer u8Layer(std::string name, std::vector<uint8_t> values) {
  TestLayer layer;
  layer.name = std::move(name);
  layer.type = 0;
  layer.u8 = std::move(values);
  return layer;
}

// Current NavMapLayer layout (also the element layout of NavMap.layers).
void writeLayer(CdrWriter& w, const TestLayer& l, uint64_t geometry_id) {
  w.putHeader(l.sec, l.nanosec, "map");
  w.putString(l.name);
  w.put<uint8_t>(l.type);
  w.putString("description");
  w.putString(l.unit);
  w.put<uint8_t>(l.has_unknown ? 1 : 0);
  w.put<double>(l.unknown_value);
  w.put<uint64_t>(geometry_id);
  w.putSeq(l.indices);
  w.putSeq(l.u8);
  w.putSeq(l.f32);
  w.putSeq(l.f64);
  w.putSeq(l.i16);
}

std::vector<uint8_t> serializeLayerMessage(const TestLayer& layer, uint64_t geometry_id) {
  CdrWriter w;
  writeLayer(w, layer, geometry_id);
  return w.bytes();
}

struct TestLink {
  uint32_t from = 0;
  uint32_t to = 0;
  uint8_t type = 0;
  float length = 1.5F;
};

// Two unit squares (4 triangles, 6 vertices): navcels 0-1 on surface "map",
// 2-3 on surface "upstairs", 1 m above.
struct TestMap {
  std::vector<float> x{0, 1, 1, 0, 0, 1};
  std::vector<float> y{0, 0, 1, 1, 0, 0};
  std::vector<float> z{0, 0, 0, 0, 1, 1};
  bool has_rgba = false;
  std::vector<uint8_t> r, g, b, a;
  std::vector<uint32_t> v0{0, 0, 4, 4};
  std::vector<uint32_t> v1{1, 2, 5, 1};
  std::vector<uint32_t> v2{2, 3, 1, 0};
  std::vector<std::pair<std::string, std::vector<uint32_t>>> surfaces{{"map", {0, 1}}, {"upstairs", {2, 3}}};
  std::vector<TestLayer> layers;
  uint64_t geometry_id = 0xFEDCBA9876543210ULL;
  std::vector<TestLink> links;
  std::string frame = "map";
  int32_t stamp_sec = 12;
  uint32_t stamp_nanosec = 500'000'000;

  [[nodiscard]] std::vector<uint8_t> serializeCurrent() const {
    CdrWriter w;
    writeGeometry(w);
    w.put<uint32_t>(static_cast<uint32_t>(layers.size()));
    for (const auto& l : layers) {
      writeLayer(w, l, geometry_id);
    }
    w.put<uint64_t>(geometry_id);
    w.put<uint32_t>(static_cast<uint32_t>(links.size()));
    for (const auto& link : links) {
      w.put<uint32_t>(link.from);
      w.put<uint32_t>(link.to);
      w.put<uint8_t>(link.type);
      w.put<float>(link.length);
      w.put<uint8_t>(1);
    }
    return w.bytes();
  }

  [[nodiscard]] std::vector<uint8_t> serializeV1() const {
    CdrWriter w;
    writeGeometry(w);
    w.put<uint32_t>(static_cast<uint32_t>(layers.size()));
    for (const auto& l : layers) {
      w.putHeader(0, 0, "map");
      w.putString(l.name);
      w.put<uint8_t>(l.type);
      w.putSeq(l.u8);
      w.putSeq(l.f32);
      w.putSeq(std::vector<double>{});
    }
    return w.bytes();
  }

 private:
  void writeGeometry(CdrWriter& w) const {
    w.putHeader(stamp_sec, stamp_nanosec, frame);
    w.putSeq(x);
    w.putSeq(y);
    w.putSeq(z);
    w.put<uint8_t>(has_rgba ? 1 : 0);
    w.putSeq(r);
    w.putSeq(g);
    w.putSeq(b);
    w.putSeq(a);
    w.putSeq(v0);
    w.putSeq(v1);
    w.putSeq(v2);
    w.put<uint32_t>(static_cast<uint32_t>(surfaces.size()));
    for (const auto& [surface_frame, cells] : surfaces) {
      w.putString(surface_frame);
      w.putSeq(cells);
    }
  }
};

PJ::Span<const uint8_t> span(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// The bundle an MCAP recorded with the current interfaces carries for a
// NavMapLayer topic.
constexpr std::string_view kLayerSchema = R"(# Per-NavCel layer (one scalar per triangle)
std_msgs/Header header
uint8 U8 = 0
uint8 F32 = 1
uint8 F64 = 2
uint8 I16 = 3
string name
uint8 type
string description
string unit
bool has_unknown
float64 unknown_value
uint64 geometry_id
uint32[] indices
uint8[]   data_u8
float32[] data_f32
float64[] data_f64
int16[]   data_i16
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
)";

using Target = PJ::sdk::MeshChannel::Target;
using Datatype = PJ::sdk::PointField::Datatype;

// Element `index`, component `component`, of a tightly packed array, widened to double.
double valueAt(
    PJ::Span<const uint8_t> data, Datatype datatype, uint32_t count, uint32_t index, uint32_t component = 0) {
  const uint8_t* p =
      data.data() + ((static_cast<size_t>(index) * count) + component) * PJ::sdk::bytesPerElement(datatype);
  switch (datatype) {
    case Datatype::kUint8:
      return *p;
    case Datatype::kInt16: {
      int16_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case Datatype::kUint32: {
      uint32_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case Datatype::kFloat64: {
      double v = 0;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case Datatype::kFloat32: {
      float v = 0;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    default:
      ADD_FAILURE() << "unexpected datatype";
      return std::nan("");
  }
}

// A numeric field of a recorded scalar row; NaN (and a failure) when it is missing.
template <typename Row>
double numericField(const Row& row, std::string_view name) {
  const auto* field = PJ::sdk::testing::ParserWriteRecorder::findField(row, name);
  EXPECT_NE(field, nullptr) << name;
  return field != nullptr ? field->numeric : std::nan("");
}

const PJ::sdk::MeshChannel& channel(const PJ::sdk::MeshMap& mesh, Target target, std::string_view name) {
  const auto* found = PJ::sdk::findChannel(mesh.channels, target, name);
  EXPECT_NE(found, nullptr) << "no channel " << name;
  static const PJ::sdk::MeshChannel kMissing;
  return found != nullptr ? *found : kMissing;
}

double faceValue(const PJ::sdk::MeshMap& mesh, std::string_view name, uint32_t face) {
  const auto& c = channel(mesh, Target::kFace, name);
  return c.data.empty() ? std::nan("") : valueAt(c.data, c.datatype, c.count, face);
}

double vertexValue(const PJ::sdk::MeshMap& mesh, std::string_view name, uint32_t vertex) {
  const auto& c = channel(mesh, Target::kVertex, name);
  return c.data.empty() ? std::nan("") : valueAt(c.data, c.datatype, c.count, vertex);
}

double position(const PJ::sdk::MeshMap& mesh, uint32_t vertex, uint32_t axis) {
  return valueAt(mesh.positions, mesh.position_type, 3, vertex, axis);
}

uint32_t cornerIndex(const PJ::sdk::MeshMap& mesh, uint32_t face, uint32_t corner) {
  return static_cast<uint32_t>(valueAt(mesh.indices, Datatype::kUint32, 3, face, corner));
}

class NavMapParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto lib = PJ::MessageParserLibrary::load(PJ_NAVMAP_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library_ = std::move(*lib);
    handle_ = library_.createHandle();
    ASSERT_TRUE(handle_.valid());
    registry_.registerService<PJ::sdk::ParserWriteHostService>(recorder_.makeHost());
    registry_.registerService<PJ::sdk::ParserRuntimeHostService>(runtime_.makeHost());
    ASSERT_TRUE(handle_.bind(registry_.view()));
  }

  void bind(std::string_view type, std::string_view schema) {
    auto status = handle_.bindSchema(type, span(schema));
    ASSERT_TRUE(status) << status.error();
  }

  // The real host path: the functional C extension, which serializes the
  // object through its canonical codec (and so runs validateMeshMap*()).
  template <typename T>
  T parseObject(const std::vector<uint8_t>& payload) {
    auto record = handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size()));
    EXPECT_TRUE(record) << (record ? "" : record.error());
    if (!record) {
      return {};
    }
    const auto* object = pj_compat::getBuiltinObject<T>(record->object);
    EXPECT_NE(object, nullptr);
    return object != nullptr ? *object : T{};
  }

  PJ::sdk::MeshMap parseMap(const std::vector<uint8_t>& payload) {
    return parseObject<PJ::sdk::MeshMap>(payload);
  }

  PJ::MessageParserLibrary library_;
  PJ::MessageParserHandle handle_{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry_;
  PJ::sdk::testing::ParserWriteRecorder recorder_;
  PJ::sdk::testing::ParserRuntimeRecorder runtime_;
};

TEST_F(NavMapParserTest, RefusesToBindAnyOtherType) {
  // The host's wildcard ros2msg claim is accepted whenever bindSchema
  // succeeds, so a successful bind here would steal unrelated ROS topics.
  EXPECT_FALSE(handle_.bindSchema("sensor_msgs/msg/Imu", span("float64 x\n")));
  EXPECT_FALSE(handle_.bindSchema("navmap_ros_interfaces/msg/NavMapLink", span("uint32 from_navcel\n")));
  EXPECT_TRUE(handle_.bindSchema("navmap_ros_interfaces/NavMap", span(kCurrentSchema)));
  EXPECT_TRUE(handle_.bindSchema(kNavMapType, span(kCurrentSchema)));
  EXPECT_TRUE(handle_.bindSchema(kNavMapLayerType, span(kLayerSchema)));
}

TEST_F(NavMapParserTest, ClaimsBothRoutesOfTheTwoTypes) {
  const auto* claims =
      static_cast<const PJ_parser_route_claims_v1_t*>(handle_.getPluginExtension(PJ_PARSER_ROUTE_CLAIMS_EXTENSION_V1));
  ASSERT_NE(claims, nullptr);
  auto classify = [&](std::string_view type) {
    PJ_route_classification_v1_t out{};
    PJ_error_t error{};
    const PJ_bytes_view_t schema{reinterpret_cast<const uint8_t*>(kCurrentSchema.data()), kCurrentSchema.size()};
    EXPECT_TRUE(claims->classify_routes(handle_.context(), {type.data(), type.size()}, schema, &out, &error));
    return out;
  };
  const auto map = classify(kNavMapType);
  EXPECT_EQ(map.status, PJ_PARSER_ROUTE_STATUS_CLAIMED_V1);
  EXPECT_EQ(map.route_flags, PJ_PARSER_ROUTE_FLAG_SCALAR_V1 | PJ_PARSER_ROUTE_FLAG_OBJECT_V1);
  EXPECT_EQ(map.object_type, static_cast<uint16_t>(PJ::sdk::BuiltinObjectType::kMeshMap));
  const auto layer = classify(kNavMapLayerType);
  EXPECT_EQ(layer.status, PJ_PARSER_ROUTE_STATUS_CLAIMED_V1);
  EXPECT_EQ(layer.object_type, static_cast<uint16_t>(PJ::sdk::BuiltinObjectType::kMeshMapUpdate));
  EXPECT_EQ(classify("sensor_msgs/msg/Imu").status, PJ_PARSER_ROUTE_STATUS_DECLINED_V1);
}

TEST_F(NavMapParserTest, BuildsTheMeshWithChannelsAndLinks) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("cost", {0.0F, 2.0F, 4.0F, 1.0F}));
  map.links.push_back({.from = 0, .to = 2, .type = 2});
  const auto mesh = parseMap(map.serializeCurrent());

  EXPECT_EQ(mesh.timestamp_ns, kReceiveTime);
  EXPECT_EQ(mesh.frame_id, "map");
  EXPECT_EQ(mesh.geometry_id, "18364758544493064720");  // 0xFEDCBA9876543210
  ASSERT_EQ(mesh.vertex_count, 6u);
  ASSERT_EQ(mesh.face_count, 4u);
  EXPECT_EQ(mesh.position_type, Datatype::kFloat32);
  EXPECT_DOUBLE_EQ(position(mesh, 2, 0), 1.0);
  EXPECT_DOUBLE_EQ(position(mesh, 2, 1), 1.0);
  EXPECT_DOUBLE_EQ(position(mesh, 4, 2), 1.0);
  EXPECT_EQ(cornerIndex(mesh, 2, 0), 4u);  // navcel 2 = (4, 5, 1)
  EXPECT_EQ(cornerIndex(mesh, 2, 1), 5u);
  EXPECT_EQ(cornerIndex(mesh, 2, 2), 1u);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kVertex, "red"), nullptr);  // no vertex colours

  const auto& surface = channel(mesh, Target::kFace, "surface");
  EXPECT_EQ(surface.info.kind, PJ::sdk::ChannelKind::kCategorical);
  EXPECT_FALSE(surface.info.no_data.has_value());  // every NavCel is on a surface
  ASSERT_EQ(surface.info.categories.size(), 2u);
  EXPECT_EQ(surface.info.categories[1].label, "upstairs");
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 1), 0.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 2), 1.0);

  const auto& cost = channel(mesh, Target::kFace, "cost");
  EXPECT_EQ(cost.datatype, Datatype::kFloat32);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 2), 4.0);

  ASSERT_EQ(mesh.links.size(), 1u);
  EXPECT_EQ(mesh.links[0].from_face, 0u);
  EXPECT_EQ(mesh.links[0].to_face, 2u);
  EXPECT_EQ(mesh.links[0].kind, 2u);
  EXPECT_TRUE(mesh.links[0].bidirectional);
  EXPECT_DOUBLE_EQ(mesh.links[0].cost, 1.5);  // the link's length
}

TEST_F(NavMapParserTest, VertexColorsBecomeChannels) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.has_rgba = true;
  map.r = {10, 20, 30, 40, 50, 60};
  map.g = {1, 2, 3, 4, 5, 6};
  map.b = {0, 0, 0, 0, 0, 0};
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(channel(mesh, Target::kVertex, "red").datatype, Datatype::kUint8);
  EXPECT_DOUBLE_EQ(vertexValue(mesh, "red", 3), 40.0);
  EXPECT_DOUBLE_EQ(vertexValue(mesh, "green", 5), 6.0);
  EXPECT_DOUBLE_EQ(vertexValue(mesh, "alpha", 0), 255.0);  // colors_a empty: opaque
}

TEST_F(NavMapParserTest, LayersKeepTheirTypeUnitAndUnknownValue) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  TestLayer occupancy = u8Layer("occupancy", {0, 254, 255, 7});
  occupancy.has_unknown = true;
  occupancy.unknown_value = 255;
  map.layers.push_back(occupancy);

  TestLayer log_odds;
  log_odds.name = "occ_logodds";
  log_odds.type = 3;
  log_odds.unit = "1/1000 log-odds";
  log_odds.has_unknown = true;
  log_odds.unknown_value = -32768;
  log_odds.i16 = {0, -32768, 2000, -2000};
  map.layers.push_back(log_odds);

  TestLayer height;
  height.name = "height";
  height.type = 3;
  height.has_unknown = true;
  height.unknown_value = -1.0;
  height.indices = {0, 1};  // sparse inside a full map: navcels 2-3 have no data
  height.i16 = {-1, 7};
  map.layers.push_back(height);

  TestLayer slope = f32Layer("slope", {0.5F, 0.25F});
  slope.indices = {3, 1};  // sparse float: unlisted NavCels become NaN
  map.layers.push_back(slope);

  TestLayer ids;
  ids.name = "ids";
  ids.type = 3;
  ids.indices = {2};  // sparse integer with no unknown value: nothing to fill with, dropped
  ids.i16 = {9};
  map.layers.push_back(ids);

  const auto mesh = parseMap(map.serializeCurrent());

  EXPECT_EQ(channel(mesh, Target::kFace, "occupancy").datatype, Datatype::kUint8);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occupancy", 1), 254.0);

  const auto& lo = channel(mesh, Target::kFace, "occ_logodds");
  EXPECT_EQ(lo.datatype, Datatype::kInt16);  // raw, no conversion
  EXPECT_EQ(lo.info.unit, "1/1000 log-odds");
  ASSERT_TRUE(lo.info.no_data.has_value());
  EXPECT_DOUBLE_EQ(*lo.info.no_data, -32768.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occ_logodds", 2), 2000.0);

  EXPECT_EQ(channel(mesh, Target::kFace, "height").datatype, Datatype::kInt16);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "height", 1), 7.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "height", 2), -1.0);  // unlisted: the unknown value

  EXPECT_EQ(channel(mesh, Target::kFace, "slope").datatype, Datatype::kFloat32);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "slope", 3), 0.5);
  EXPECT_TRUE(std::isnan(faceValue(mesh, "slope", 0)));

  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "ids"), nullptr);
}

TEST_F(NavMapParserTest, ALayerTakesTheTypeOfTheArrayItCarries) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  TestLayer mismatched = f32Layer("mixed", {1.0F, std::nanf(""), 3.0F, 4.0F});
  mismatched.type = 3;  // declared int16, but only data_f32 is filled
  map.layers.push_back(mismatched);
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(channel(mesh, Target::kFace, "mixed").datatype, Datatype::kFloat32);
  EXPECT_TRUE(std::isnan(faceValue(mesh, "mixed", 1)));  // not cast to an integer
}

TEST_F(NavMapParserTest, DropsALayerThatDoesNotMatchTheMesh) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("short", {1.0F}));  // 1 value for 4 navcels
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(mesh.face_count, 4u);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "short"), nullptr);
}

TEST_F(NavMapParserTest, DecodesTheFirstReleasedLayout) {
  bind(kNavMapType, kV1Schema);
  TestMap map;
  map.layers.push_back(f32Layer("cost", {0.0F, 1.0F, 2.0F, 3.0F}));
  const auto mesh = parseMap(map.serializeV1());
  EXPECT_EQ(mesh.face_count, 4u);
  EXPECT_EQ(mesh.geometry_id, "");  // V1 has no geometry id
  EXPECT_TRUE(mesh.links.empty());
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 2), 2.0);
}

TEST_F(NavMapParserTest, FallsBackToTheCurrentLayoutWithoutSchema) {
  bind(kNavMapType, "");
  TestMap map;
  map.links.push_back({.from = 1, .to = 3, .type = 0});
  map.layers.push_back(f32Layer("cost", {0.0F, 1.0F, 2.0F, 3.0F}));
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(mesh.links.size(), 1u);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 3), 3.0);

  bind(kNavMapLayerType, "");
  const auto update =
      parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(f32Layer("cost", {1.0F, 2.0F, 3.0F, 4.0F}), 7));
  ASSERT_EQ(update.patches.size(), 1u);
  EXPECT_EQ(update.patches[0].channel.data.size(), 16u);
}

TEST_F(NavMapParserTest, MapWithoutSurfacesHasNoSurfaceChannel) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.surfaces.clear();
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(mesh.frame_id, "map");
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "surface"), nullptr);
  EXPECT_EQ(mesh.face_count, 4u);
}

TEST_F(NavMapParserTest, LayerMessageBecomesAPatch) {
  bind(kNavMapLayerType, kLayerSchema);

  // A full layer: a dense patch, no indices, in the layer's own type.
  TestLayer occupancy = u8Layer("occupancy", {0, 9, 254});
  occupancy.has_unknown = true;
  occupancy.unknown_value = 255;
  auto update = parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(occupancy, 42));
  EXPECT_EQ(update.frame_id, "map");
  EXPECT_EQ(update.geometry_id, "42");
  ASSERT_EQ(update.patches.size(), 1u);
  const auto& dense = update.patches[0];
  EXPECT_EQ(dense.channel.name, "occupancy");
  EXPECT_EQ(dense.channel.target, Target::kFace);
  EXPECT_EQ(dense.channel.datatype, Datatype::kUint8);
  EXPECT_TRUE(dense.indices.empty());
  EXPECT_DOUBLE_EQ(valueAt(dense.channel.data, Datatype::kUint8, 1, 1), 9.0);

  // A sparse layer with an unknown value: the listed NavCels, the unknown value as no-data.
  TestLayer sparse;
  sparse.name = "height";
  sparse.type = 3;
  sparse.unit = "cm";
  sparse.has_unknown = true;
  sparse.unknown_value = -1;
  sparse.indices = {7, 3};
  sparse.i16 = {5, -1};
  update = parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(sparse, 0));
  EXPECT_EQ(update.geometry_id, "");  // 0 = unspecified
  ASSERT_EQ(update.patches.size(), 1u);
  const auto& patch = update.patches[0];
  EXPECT_EQ(patch.channel.datatype, Datatype::kInt16);
  EXPECT_EQ(patch.channel.info.unit, "cm");
  ASSERT_TRUE(patch.channel.info.no_data.has_value());
  EXPECT_DOUBLE_EQ(*patch.channel.info.no_data, -1.0);
  ASSERT_EQ(patch.indices.size(), 8u);
  EXPECT_DOUBLE_EQ(valueAt(patch.indices, Datatype::kUint32, 1, 0), 7.0);
  EXPECT_DOUBLE_EQ(valueAt(patch.channel.data, Datatype::kInt16, 1, 1), -1.0);

  // Indices and data that disagree are rejected.
  sparse.indices = {1};
  const auto mismatched = serializeLayerMessage(sparse, 0);
  EXPECT_FALSE(
      handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(mismatched.data(), mismatched.size())));
}

TEST_F(NavMapParserTest, EmitsSummarySeriesOnTheScalarRoute) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("cost", {0.0F, 1.0F, 2.0F, 3.0F}));
  map.links.push_back({.from = 0, .to = 2, .type = 2});
  const auto payload = map.serializeCurrent();
  ASSERT_TRUE(handle_.parse(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size())));
  ASSERT_EQ(recorder_.rows().size(), 1u);
  const auto& row = recorder_.rows()[0];
  EXPECT_EQ(row.timestamp, kReceiveTime);
  auto field = [&](std::string_view name) {
    const auto* f = PJ::sdk::testing::ParserWriteRecorder::findField(row, name);
    EXPECT_NE(f, nullptr) << name;
    return f != nullptr ? *f : PJ::sdk::testing::RecordedField{};
  };
  EXPECT_DOUBLE_EQ(field("/header/stamp").numeric, 12.5);
  EXPECT_EQ(field("/header/frame_id").string_value, "map");
  EXPECT_DOUBLE_EQ(field("/num_vertices").numeric, 6.0);
  EXPECT_DOUBLE_EQ(field("/num_navcels").numeric, 4.0);
  EXPECT_DOUBLE_EQ(field("/num_surfaces").numeric, 2.0);
  EXPECT_DOUBLE_EQ(field("/num_layers").numeric, 1.0);
  EXPECT_DOUBLE_EQ(field("/num_links").numeric, 1.0);
  EXPECT_FALSE(field("/has_vertex_rgba").bool_value);
  EXPECT_EQ(field("/geometry_id").type, PJ::PrimitiveType::kUint64);
  EXPECT_DOUBLE_EQ(field("/dropped_layers").numeric, 0.0);
  EXPECT_DOUBLE_EQ(field("/dropped_links").numeric, 0.0);

  bind(kNavMapLayerType, kLayerSchema);
  TestLayer sparse = f32Layer("cost", {1.0F, 2.0F});
  sparse.indices = {0, 3};
  const auto layer_payload = serializeLayerMessage(sparse, 3);
  ASSERT_TRUE(handle_.parse(kReceiveTime, PJ::Span<const uint8_t>(layer_payload.data(), layer_payload.size())));
  ASSERT_EQ(recorder_.rows().size(), 2u);
  const auto& layer_row = recorder_.rows()[1];
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(layer_row, "/name")->string_value, "cost");
  EXPECT_DOUBLE_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(layer_row, "/num_values")->numeric, 2.0);
  EXPECT_TRUE(PJ::sdk::testing::ParserWriteRecorder::findField(layer_row, "/sparse")->bool_value);
}

TEST_F(NavMapParserTest, RejectsMalformedMessages) {
  bind(kNavMapType, kCurrentSchema);
  TestMap bad_index;
  bad_index.v2[0] = 99;
  auto payload = bad_index.serializeCurrent();
  EXPECT_FALSE(handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size())));

  // A schema that references an undefined type cannot bind.
  EXPECT_FALSE(handle_.bindSchema(kNavMapType, span("pkg/Missing thing\n")));
}

TEST_F(NavMapParserTest, RejectsEveryTruncatedPrefix) {
  // nanocdr trims alignment padding unchecked; the reader must refuse each
  // prefix before nanocdr can read past it (run under ASan to see a miss).
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("cost", {0.0F, 1.0F, 2.0F, 3.0F}));
  map.links.push_back({.from = 0, .to = 2, .type = 2});
  const auto full = map.serializeCurrent();
  for (size_t size = 0; size < full.size(); ++size) {
    // Each prefix gets its own allocation, so a read past it is out of bounds.
    const std::vector<uint8_t> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size));
    const PJ::Span<const uint8_t> bytes(prefix.data(), prefix.size());
    EXPECT_FALSE(handle_.parseObjectFunctional(kReceiveTime, bytes)) << "prefix " << size;
    EXPECT_FALSE(handle_.parse(kReceiveTime, bytes)) << "prefix " << size;
  }
  EXPECT_TRUE(handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(full.data(), full.size())));
}

TEST_F(NavMapParserTest, RejectsBigEndianAndOversizedSequences) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  auto payload = map.serializeCurrent();
  payload[1] = 0x00;  // CDR_BE
  auto record = handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size()));
  ASSERT_FALSE(record);
  EXPECT_NE(record.error().find("big-endian"), std::string::npos);

  // header (stamp, "map") then positions_x claiming 0xFFFFFFFF floats.
  CdrWriter w;
  w.putHeader(0, 0, "map");
  w.put<uint32_t>(0xFFFFFFFFU);
  record = handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(w.bytes().data(), w.bytes().size()));
  ASSERT_FALSE(record);
  EXPECT_NE(record.error().find("sequence length"), std::string::npos);
}

// --- use_embedded_timestamp ---------------------------------------------------

TEST_F(NavMapParserTest, EmbeddedTimestampMovesObjectsAndScalarsToTheHeaderStamp) {
  ASSERT_TRUE(handle_.loadConfig(R"({"use_embedded_timestamp": true})"));
  std::string saved;
  ASSERT_TRUE(handle_.saveConfig(saved));
  EXPECT_NE(saved.find(R"("use_embedded_timestamp":true)"), std::string::npos) << saved;

  bind(kNavMapType, kCurrentSchema);
  const auto map_payload = TestMap{}.serializeCurrent();  // header stamp 12.5 s
  const PJ::Span<const uint8_t> map_bytes(map_payload.data(), map_payload.size());
  auto record = handle_.parseObjectFunctional(kReceiveTime, map_bytes);
  ASSERT_TRUE(record) << record.error();
  EXPECT_EQ(record->ts.value_or(kReceiveTime), 12'500'000'000);
  const auto* mesh = pj_compat::getBuiltinObject<PJ::sdk::MeshMap>(record->object);
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->timestamp_ns, 12'500'000'000);
  ASSERT_TRUE(handle_.parse(kReceiveTime, map_bytes));
  ASSERT_EQ(recorder_.rows().size(), 1u);
  EXPECT_EQ(recorder_.rows()[0].timestamp, 12'500'000'000);

  bind(kNavMapLayerType, kLayerSchema);
  auto parseLayerAt = [&](int32_t sec, uint32_t nanosec) {
    TestLayer layer = f32Layer("cost", {1.0F, 2.0F});
    layer.sec = sec;
    layer.nanosec = nanosec;
    const auto payload = serializeLayerMessage(layer, 0);
    const PJ::Span<const uint8_t> bytes(payload.data(), payload.size());
    auto update_record = handle_.parseObjectFunctional(kReceiveTime, bytes);
    EXPECT_TRUE(update_record) << (update_record ? "" : update_record.error());
    EXPECT_TRUE(handle_.parse(kReceiveTime, bytes));
    const auto* update =
        update_record ? pj_compat::getBuiltinObject<PJ::sdk::MeshMapUpdate>(update_record->object) : nullptr;
    EXPECT_NE(update, nullptr);
    return std::tuple{
        update_record ? update_record->ts.value_or(kReceiveTime) : 0, update != nullptr ? update->timestamp_ns : 0,
        recorder_.rows().back().timestamp};
  };
  EXPECT_EQ(parseLayerAt(3, 250), std::tuple(3'000'000'250, 3'000'000'250, 3'000'000'250));
  // A non-positive stamp keeps the receive time.
  EXPECT_EQ(parseLayerAt(0, 0), std::tuple(kReceiveTime, kReceiveTime, kReceiveTime));
  EXPECT_EQ(parseLayerAt(-5, 0), std::tuple(kReceiveTime, kReceiveTime, kReceiveTime));
}

// ROS requires nanosec < 1 s; a corrupt stamp must not replace the receive time.
TEST_F(NavMapParserTest, InvalidEmbeddedStampKeepsTheReceiveTime) {
  ASSERT_TRUE(handle_.loadConfig(R"({"use_embedded_timestamp": true})"));
  bind(kNavMapType, kCurrentSchema);
  auto stampsOf = [&](uint32_t nanosec) {
    TestMap map;
    map.stamp_sec = 1;
    map.stamp_nanosec = nanosec;
    const auto payload = map.serializeCurrent();
    const PJ::Span<const uint8_t> bytes(payload.data(), payload.size());
    auto record = handle_.parseObjectFunctional(kReceiveTime, bytes);
    EXPECT_TRUE(record) << (record ? "" : record.error());
    EXPECT_TRUE(handle_.parse(kReceiveTime, bytes));
    const auto* mesh = record ? pj_compat::getBuiltinObject<PJ::sdk::MeshMap>(record->object) : nullptr;
    EXPECT_NE(mesh, nullptr);
    return std::tuple{
        record ? record->ts.value_or(kReceiveTime) : 0, mesh != nullptr ? mesh->timestamp_ns : 0,
        recorder_.rows().back().timestamp};
  };
  EXPECT_EQ(stampsOf(1'500'000'000), std::tuple(kReceiveTime, kReceiveTime, kReceiveTime));
  EXPECT_EQ(stampsOf(1'000'000'000), std::tuple(kReceiveTime, kReceiveTime, kReceiveTime));
  // The largest valid nanosec is still honoured.
  EXPECT_EQ(stampsOf(999'999'999), std::tuple(1'999'999'999, 1'999'999'999, 1'999'999'999));
}

TEST_F(NavMapParserTest, ReceiveTimeIsTheDefault) {
  bind(kNavMapLayerType, kLayerSchema);
  TestLayer layer = f32Layer("cost", {1.0F});
  layer.sec = 3;
  const auto payload = serializeLayerMessage(layer, 0);
  auto record = handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size()));
  ASSERT_TRUE(record) << record.error();
  EXPECT_EQ(record->ts.value_or(kReceiveTime), kReceiveTime);
  const auto* update = pj_compat::getBuiltinObject<PJ::sdk::MeshMapUpdate>(record->object);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(update->timestamp_ns, kReceiveTime);
}

// --- Hostile schemas ------------------------------------------------------------

// A message type with no fields is one uint8 on the wire (what rosidl writes),
// so a fixed array of them is bounded by the payload. The first count is the
// largest a schema can declare: decoding could never finish if the elements
// consumed no bytes.
TEST(NavMapDecoderTest, FieldlessElementsCannotOutrunThePayload) {
  const std::vector<uint8_t> four_bytes{0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0};
  auto decode = [](std::string_view bundle, const std::vector<uint8_t>& payload) {
    auto schema = navmap_parser::compileMsgBundle("pkg/Root", bundle);
    EXPECT_TRUE(schema) << (schema ? "" : schema.error());
    navmap_parser::CdrReader reader(payload.data(), payload.size());
    return std::pair{navmap_parser::decodeMessage(*schema, reader, true), reader.remaining()};
  };
  EXPECT_THROW(
      (void)decode("pkg/E[4294967295] e\n===\nMSG: pkg/E\nuint8 C = 1\n", four_bytes), navmap_parser::CdrError);
  // Nested fixed arrays multiply the count.
  EXPECT_THROW(
      (void)decode("pkg/F[65536] f\n===\nMSG: pkg/F\npkg/E[65536] e\n===\nMSG: pkg/E\nuint8 C = 1\n", four_bytes),
      navmap_parser::CdrError);

  const auto [node, remaining] =
      decode("pkg/E[2] e\nuint8 after\n===\nMSG: pkg/E\nuint8 C = 1\n", {0x00, 0x01, 0x00, 0x00, 0, 0, 7});
  ASSERT_NE(node.find("e"), nullptr);
  EXPECT_EQ(node.find("e")->count, 2u);
  ASSERT_NE(node.find("after"), nullptr);
  EXPECT_DOUBLE_EQ(node.find("after")->number, 7.0);
  EXPECT_EQ(remaining, 0u);
}

TEST(NavMapDecoderTest, ComputesTheMinimumWireSizeOfEachType) {
  auto schema = navmap_parser::compileMsgBundle(kNavMapType, kCurrentSchema);
  ASSERT_TRUE(schema) << schema.error();
  EXPECT_EQ(schema->min_wire_size.at("navmap_ros_interfaces/NavMapLink"), 14u);    // 4 + 4 + 1 + 4 + 1
  EXPECT_EQ(schema->min_wire_size.at("navmap_ros_interfaces/NavMapSurface"), 9u);  // (4 + 1) + 4
  EXPECT_EQ(schema->min_wire_size.at("std_msgs/Header"), 13u);                     // 4 + 4 + (4 + 1)
}

TEST_F(NavMapParserTest, BoundsNestedSequencesByTheirMinimumWireSize) {
  bind(kNavMapType, kCurrentSchema);
  auto payload = TestMap{}.serializeCurrent();  // ends with the link count, 0
  // 20 more bytes: room for 20 one-byte elements, not for 20 links of at least 14 bytes.
  const uint32_t links = 20;
  std::memcpy(payload.data() + payload.size() - sizeof(links), &links, sizeof(links));
  payload.resize(payload.size() + links, 0);
  const auto record =
      handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size()));
  ASSERT_FALSE(record);
  EXPECT_NE(record.error().find("sequence length"), std::string::npos) << record.error();
}

// An acyclic chain of embedded types: the root embeds T0, T0 embeds T1, and so on.
std::string chainBundle(int length) {
  std::string bundle = "pkg/T0 next\n";
  for (int i = 0; i < length; ++i) {
    bundle += "===\nMSG: pkg/T" + std::to_string(i) + "\n";
    bundle += i + 1 < length ? "pkg/T" + std::to_string(i + 1) + " next\n" : "uint8 leaf\n";
  }
  return bundle;
}

// Compiling a schema walks the embedded types recursively, so a long chain of
// distinct types (no cycle to stop it) must hit the nesting limit decodeNode()
// enforces instead of overflowing the stack.
TEST_F(NavMapParserTest, BindingRejectsSchemasNestedDeeperThanTheDecoderAllows) {
  EXPECT_TRUE(handle_.bindSchema(kNavMapType, span(chainBundle(32))));  // root + 32 levels, the decoder's depth 32
  for (const int length : {33, 40, 100'000}) {
    const auto bundle = chainBundle(length);
    const auto status = handle_.bindSchema(kNavMapType, span(bundle));
    ASSERT_FALSE(status) << length;
    EXPECT_NE(status.error().find("deeper than 32"), std::string::npos) << status.error();
  }
}

// A fixed-size array skips the sequence-length check, so a hostile schema could
// make the parser reserve gigabytes (float64[4294967295] is 32 GiB) before the
// missing data was noticed. The bounds check must reject it first.
TEST_F(NavMapParserTest, HugeFixedArraysAreRejectedBeforeAnyAllocation) {
  const std::vector<uint8_t> header_only{0x00, 0x01, 0x00, 0x00};
  const PJ::Span<const uint8_t> bytes(header_only.data(), header_only.size());
  for (const std::string_view field : {"float64[4294967295] positions_x\n", "string[4294967295] positions_x\n"}) {
    bind(kNavMapType, field);
    auto record = handle_.parseObjectFunctional(kReceiveTime, bytes);
    ASSERT_FALSE(record) << field;
    EXPECT_NE(record.error().find("fixed array"), std::string::npos) << record.error();
    EXPECT_FALSE(handle_.parse(kReceiveTime, bytes)) << field;
  }
}

// Index-like fields declared as a floating type: NaN or out-of-range values are
// invalid (casting them would be undefined), so their link or layer is dropped.
constexpr std::string_view kFloatIndexSchema = R"(std_msgs/Header header
float32[] positions_x
float32[] positions_y
float32[] positions_z
uint32[] navcels_v0
uint32[] navcels_v1
uint32[] navcels_v2
navmap_ros_interfaces/NavMapLayer[] layers
navmap_ros_interfaces/NavMapLink[] links
================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
string name
float64 type
float64[] indices
float32[] data_f32
================================================================================
MSG: navmap_ros_interfaces/NavMapLink
float64 from_navcel
float64 to_navcel
float64 type
)";

TEST_F(NavMapParserTest, InvalidFloatingIndexValuesAreDropped) {
  bind(kNavMapType, kFloatIndexSchema);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  CdrWriter w;
  w.putHeader(0, 0, "map");
  w.putSeq(std::vector<float>{0, 1, 0});
  w.putSeq(std::vector<float>{0, 0, 1});
  w.putSeq(std::vector<float>{0, 0, 0});
  w.putSeq(std::vector<uint32_t>{0});
  w.putSeq(std::vector<uint32_t>{1});
  w.putSeq(std::vector<uint32_t>{2});
  struct Layer {
    std::string name;
    double type;
    std::vector<double> indices;
    float value;
  };
  const std::vector<Layer> layers{
      {"nan_type", nan, {}, 1.5F}, {"nan_index", 1, {nan}, 2.0F}, {"huge_index", 1, {1e300}, 3.0F}};
  w.put<uint32_t>(static_cast<uint32_t>(layers.size()));
  for (const auto& l : layers) {
    w.putString(l.name);
    w.put<double>(l.type);
    w.putSeq(l.indices);
    w.putSeq(std::vector<float>{l.value});
  }
  const std::vector<std::array<double, 3>> links{{nan, 0, 1}, {0, 0, 1e10}, {0, -1, 1}, {0, 0, 2}};
  w.put<uint32_t>(static_cast<uint32_t>(links.size()));
  for (const auto& [from, to, type] : links) {
    w.put<double>(from);
    w.put<double>(to);
    w.put<double>(type);
  }
  const auto mesh = parseMap(w.bytes());
  EXPECT_EQ(mesh.face_count, 1u);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "nan_type", 0), 1.5);  // an unknown type tag: the array present decides
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "nan_index"), nullptr);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "huge_index"), nullptr);
  ASSERT_EQ(mesh.links.size(), 1u);
  EXPECT_EQ(mesh.links[0].kind, 2u);

  ASSERT_TRUE(handle_.parse(kReceiveTime, PJ::Span<const uint8_t>(w.bytes().data(), w.bytes().size())));
  const auto& row = recorder_.rows().back();
  EXPECT_DOUBLE_EQ(numericField(row, "/dropped_layers"), 2.0);
  EXPECT_DOUBLE_EQ(numericField(row, "/dropped_links"), 3.0);
}

// --- Channels -------------------------------------------------------------------

TEST_F(NavMapParserTest, NavCelsOnNoSurfaceGetTheNoDataSentinel) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.v0 = {0, 0, 4};
  map.v1 = {1, 2, 5};
  map.v2 = {2, 3, 1};
  map.surfaces = {{"map", {2}}};
  const auto mesh = parseMap(map.serializeCurrent());
  const auto& surface = channel(mesh, Target::kFace, "surface");
  EXPECT_EQ(surface.datatype, Datatype::kUint32);
  ASSERT_TRUE(surface.info.no_data.has_value());
  EXPECT_DOUBLE_EQ(*surface.info.no_data, 4294967295.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 0), 4294967295.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 1), 4294967295.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 2), 0.0);
}

// A sparse layer inside a map needs a fill value for the NavCels it does not
// list; an integer layer whose "unknown" value its type cannot hold has none.
TEST_F(NavMapParserTest, SparseIntegerLayerWithoutAStorableUnknownIsDropped) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  TestLayer occupancy = u8Layer("occupancy", {7});
  occupancy.has_unknown = true;
  occupancy.unknown_value = -1;  // not a uint8
  occupancy.indices = {1};
  map.layers.push_back(occupancy);
  TestLayer height;
  height.name = "height";
  height.type = 3;
  height.has_unknown = true;
  height.unknown_value = 40000;  // not an int16
  height.indices = {0};
  height.i16 = {-3};
  map.layers.push_back(height);
  auto payload = map.serializeCurrent();
  auto mesh = parseMap(payload);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "occupancy"), nullptr);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "height"), nullptr);
  ASSERT_TRUE(handle_.parse(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size())));
  EXPECT_DOUBLE_EQ(numericField(recorder_.rows().back(), "/dropped_layers"), 2.0);

  // Sent dense, the same layer needs no fill: it keeps uint8 and declares no no-data value.
  map.layers = {u8Layer("occupancy", {1, 2, 3, 4})};
  map.layers[0].has_unknown = true;
  map.layers[0].unknown_value = -1;
  mesh = parseMap(map.serializeCurrent());
  const auto& dense = channel(mesh, Target::kFace, "occupancy");
  EXPECT_EQ(dense.datatype, Datatype::kUint8);
  EXPECT_FALSE(dense.info.no_data.has_value());
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occupancy", 3), 4.0);
}

TEST_F(NavMapParserTest, Float64LayersInTheMapAndInUpdates) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  TestLayer elevation;
  elevation.name = "elevation";
  elevation.type = 2;
  elevation.f64 = {0.125, 1e-3, -2.0, 1e300};
  map.layers.push_back(elevation);
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_EQ(channel(mesh, Target::kFace, "elevation").datatype, Datatype::kFloat64);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "elevation", 1), 1e-3);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "elevation", 3), 1e300);

  bind(kNavMapLayerType, kLayerSchema);
  elevation.indices = {3, 1};
  elevation.f64 = {7.5, -8.25};
  const auto update = parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(elevation, 0));
  ASSERT_EQ(update.patches.size(), 1u);
  EXPECT_EQ(update.patches[0].channel.datatype, Datatype::kFloat64);
  EXPECT_DOUBLE_EQ(valueAt(update.patches[0].channel.data, Datatype::kFloat64, 1, 1), -8.25);
}

// --- One channel name and datatype per layer, shared by maps and updates --------

TEST_F(NavMapParserTest, EmptyAndDuplicateLayerNamesAreDroppedNotRenamed) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("", {9, 9, 9, 9}));
  map.layers.push_back(f32Layer("layer", {1, 2, 3, 4}));
  map.layers.push_back(f32Layer("cost", {1, 1, 1, 1}));
  map.layers.push_back(f32Layer("cost", {2, 2, 2, 2}));
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_DOUBLE_EQ(faceValue(mesh, "layer", 3), 4.0);  // the layer named "layer", not the unnamed one
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 0), 1.0);   // the first "cost"
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "layer_2"), nullptr);
  EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, "cost_2"), nullptr);
  EXPECT_EQ(mesh.channels.size(), 3u);  // surface, layer, cost

  bind(kNavMapLayerType, kLayerSchema);
  const auto update = parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(f32Layer("layer", {5, 6, 7, 8}), 0));
  ASSERT_EQ(update.patches.size(), 1u);
  EXPECT_EQ(update.patches[0].channel.name, "layer");
  const auto unnamed = serializeLayerMessage(f32Layer("", {5, 6, 7, 8}), 0);
  EXPECT_FALSE(handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(unnamed.data(), unnamed.size())));
}

TEST_F(NavMapParserTest, ReservedLayerNamesAreDropped) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  for (const char* name : {"surface", "red", "alpha", "normal", "uv"}) {
    map.layers.push_back(f32Layer(name, {7, 7, 7, 7}));
  }
  map.layers.push_back(f32Layer("cost", {1, 2, 3, 4}));
  const auto mesh = parseMap(map.serializeCurrent());
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 2), 1.0);  // still the surface index
  for (const char* name : {"red", "alpha", "normal", "uv", "surface_layer"}) {
    EXPECT_EQ(PJ::sdk::findChannel(mesh.channels, Target::kFace, name), nullptr) << name;
  }
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 3), 4.0);

  bind(kNavMapLayerType, kLayerSchema);
  for (const char* name : {"surface", "red"}) {
    const auto payload = serializeLayerMessage(f32Layer(name, {1, 2, 3, 4}), 0);
    EXPECT_FALSE(handle_.parseObjectFunctional(kReceiveTime, PJ::Span<const uint8_t>(payload.data(), payload.size())))
        << name;
  }
}

TEST_F(NavMapParserTest, MapAndUpdateAgreeOnTheChannelDatatype) {
  auto mapChannel = [&](TestLayer layer) {
    bind(kNavMapType, kCurrentSchema);
    TestMap map;
    map.layers.push_back(std::move(layer));
    const auto mesh = parseMap(map.serializeCurrent());
    const auto* found = PJ::sdk::findChannel(mesh.channels, Target::kFace, "layer");
    return found != nullptr ? std::pair{found->datatype, found->info.no_data}
                            : std::pair{Datatype::kUnknown, std::optional<double>()};
  };
  auto updateChannel = [&](const TestLayer& layer) {
    bind(kNavMapLayerType, kLayerSchema);
    const auto update = parseObject<PJ::sdk::MeshMapUpdate>(serializeLayerMessage(layer, 0));
    EXPECT_EQ(update.patches.size(), 1u);
    return update.patches.empty()
               ? std::pair{Datatype::kUnknown, std::optional<double>()}
               : std::pair{update.patches[0].channel.datatype, update.patches[0].channel.info.no_data};
  };
  struct Case {
    uint8_t type;
    bool has_unknown;
    double unknown_value;
    Datatype expected;
    bool sparse_map_kept;  ///< false: a sparse copy in a map has nothing to fill with
  };
  for (const Case& c : {
           Case{0, false, 0, Datatype::kUint8, false},
           Case{0, true, 255, Datatype::kUint8, true},
           Case{3, true, -32768, Datatype::kInt16, true},
           Case{1, false, 0, Datatype::kFloat32, true},
       }) {
    TestLayer dense;
    dense.name = "layer";
    dense.type = c.type;
    dense.has_unknown = c.has_unknown;
    dense.unknown_value = c.unknown_value;
    TestLayer sparse = dense;
    sparse.indices = {2};
    switch (c.type) {
      case 0:
        dense.u8 = {1, 2, 3, 4};
        sparse.u8 = {5};
        break;
      case 3:
        dense.i16 = {1, 2, 3, 4};
        sparse.i16 = {5};
        break;
      default:
        dense.f32 = {1, 2, 3, 4};
        sparse.f32 = {5};
        break;
    }
    const std::string label = "type " + std::to_string(c.type) + " unknown " + std::to_string(c.unknown_value);
    const auto map_dense = mapChannel(dense);
    EXPECT_EQ(map_dense.first, c.expected) << label;
    EXPECT_EQ(updateChannel(dense), map_dense) << label;
    EXPECT_EQ(updateChannel(sparse), map_dense) << label;
    EXPECT_EQ(mapChannel(sparse).first, c.sparse_map_kept ? c.expected : Datatype::kUnknown) << label;
  }
}

// --- Dropped layers and links, upstream fidelity --------------------------------

TEST_F(NavMapParserTest, ReportsDroppedLayersAndLinks) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.layers.push_back(f32Layer("short", {1.0F}));  // 1 value for 4 NavCels
  map.layers.push_back(f32Layer("cost", {1, 2, 3, 4}));
  map.layers.push_back(f32Layer("cost", {1, 2, 3, 4}));
  map.layers.push_back(f32Layer("", {1, 2, 3, 4}));
  map.links.push_back({.from = 0, .to = 2, .type = 2});
  map.links.push_back({.from = 0, .to = 99, .type = 2});  // no NavCel 99
  const auto payload = map.serializeCurrent();
  const PJ::Span<const uint8_t> bytes(payload.data(), payload.size());

  ASSERT_TRUE(handle_.parse(kReceiveTime, bytes));
  const auto& row = recorder_.rows().back();
  EXPECT_DOUBLE_EQ(numericField(row, "/dropped_layers"), 3.0);
  EXPECT_DOUBLE_EQ(numericField(row, "/dropped_links"), 1.0);

  ASSERT_TRUE(handle_.parseObjectFunctional(kReceiveTime, bytes));
  auto reported = [&](std::string_view code) {
    return std::count_if(runtime_.diagnostics().begin(), runtime_.diagnostics().end(), [&](const auto& d) {
      return d.stable_code == code && d.level == PJ::sdk::ParserDiagnosticLevel::Warning;
    });
  };
  EXPECT_EQ(reported("parser_navmap.dropped_layers"), 1);
  EXPECT_EQ(reported("parser_navmap.dropped_links"), 1);
  ASSERT_TRUE(handle_.parseObjectFunctional(kReceiveTime, bytes));  // the same counts are not reported again
  EXPECT_EQ(reported("parser_navmap.dropped_layers"), 1);
}

// A sparse layer is stored as one element per NavCel, so a small message with
// many of them could demand gigabytes: the memory the sparse layers of a map
// expand to is capped, and a layer that would pass the cap is dropped.
TEST(NavMapDecoderTest, SparseLayersAreCappedByTheirExpandedSize) {
  auto schema = navmap_parser::compileMsgBundle(kNavMapType, kCurrentSchema);
  ASSERT_TRUE(schema) << schema.error();
  auto sparseF32 = [](std::string name) {  // 4 NavCels: expands to 16 bytes
    TestLayer layer = f32Layer(std::move(name), {1.0F});
    layer.indices = {2};
    return layer;
  };
  TestLayer flags = u8Layer("flags", {1});  // expands to 4 bytes
  flags.has_unknown = true;
  flags.indices = {0};
  TestMap map;
  map.layers = {f32Layer("dense_a", {1, 2, 3, 4}), sparseF32("s1"), sparseF32("s2"), sparseF32("s3"), flags,
                f32Layer("dense_b", {5, 6, 7, 8})};
  const auto payload = map.serializeCurrent();
  auto layerNames = [&](size_t limit) {
    navmap_parser::CdrReader reader(payload.data(), payload.size());
    auto root = navmap_parser::decodeMessage(*schema, reader, true);
    auto data = navmap_parser::extractNavMap(root, limit);
    EXPECT_TRUE(data) << (data ? "" : data.error());
    std::vector<std::string> names;
    for (const auto& layer : data->layers) {
      names.push_back(layer.name);
    }
    EXPECT_EQ(data->dropped_layers + names.size(), map.layers.size());
    return names;
  };
  using Names = std::vector<std::string>;
  // 40 bytes: s1 and s2 (32), then s3 would make 48 and is dropped, but flags (36) still fits.
  EXPECT_EQ(layerNames(40), (Names{"dense_a", "s1", "s2", "flags", "dense_b"}));
  EXPECT_EQ(layerNames(32), (Names{"dense_a", "s1", "s2", "dense_b"}));
  EXPECT_EQ(layerNames(0), (Names{"dense_a", "dense_b"}));  // dense layers cost nothing extra
  EXPECT_EQ(layerNames(navmap_parser::kMaxSparseLayerBytes), (Names{"dense_a", "s1", "s2", "s3", "flags", "dense_b"}));
}

TEST_F(NavMapParserTest, NegativeLinkLengthIsAnUnspecifiedCost) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.links.push_back({.from = 0, .to = 2, .type = 2, .length = -1.0F});  // upstream: "compute it"
  map.links.push_back({.from = 1, .to = 3, .type = 2, .length = 2.5F});
  const auto mesh = parseMap(map.serializeCurrent());
  ASSERT_EQ(mesh.links.size(), 2u);
  EXPECT_TRUE(std::isnan(mesh.links[0].cost));
  EXPECT_DOUBLE_EQ(mesh.links[1].cost, 2.5);
}

TEST_F(NavMapParserTest, EmptyHeaderFrameFallsBackToTheFirstSurface) {
  bind(kNavMapType, kCurrentSchema);
  TestMap map;
  map.frame = "";
  map.surfaces = {{"floor_0", {0, 1}}, {"floor_1", {2, 3}}};
  EXPECT_EQ(parseMap(map.serializeCurrent()).frame_id, "floor_0");
}

// --- Messages recorded from the real navmap_ros_interfaces ----------------------
//
// tests/data holds messages serialized by rclpy and recorded with rosbag2: the
// schema each MCAP channel carries (*.msg) and one raw CDR payload (*.cdr), for
// the current layout and for the first released one.

std::vector<uint8_t> readFixture(std::string_view name) {
  std::ifstream in(std::string(PJ_NAVMAP_TEST_DATA_DIR) + "/" + std::string(name), std::ios::binary);
  EXPECT_TRUE(in) << name;
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string readTextFixture(std::string_view name) {
  const auto bytes = readFixture(name);
  return {bytes.begin(), bytes.end()};
}

TEST_F(NavMapParserTest, DecodesARecordingOfTheCurrentLayout) {
  bind(kNavMapType, readTextFixture("navmap_current.msg"));
  const auto mesh = parseMap(readFixture("navmap_current.cdr"));
  EXPECT_EQ(mesh.frame_id, "map");
  EXPECT_EQ(mesh.geometry_id, "18364758544493064720");
  ASSERT_EQ(mesh.vertex_count, 7u);
  ASSERT_EQ(mesh.face_count, 5u);
  EXPECT_DOUBLE_EQ(position(mesh, 6, 0), 0.5);
  EXPECT_DOUBLE_EQ(position(mesh, 6, 1), 0.25);
  EXPECT_DOUBLE_EQ(position(mesh, 6, 2), 2.5);
  EXPECT_EQ(cornerIndex(mesh, 4, 0), 6u);
  EXPECT_DOUBLE_EQ(vertexValue(mesh, "red", 6), 70.0);
  EXPECT_DOUBLE_EQ(vertexValue(mesh, "alpha", 6), 249.0);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "surface", 4), 1.0);
  ASSERT_EQ(channel(mesh, Target::kFace, "surface").info.categories.size(), 2u);
  EXPECT_EQ(channel(mesh, Target::kFace, "surface").info.categories[1].label, "floor_1");

  const auto& occupancy = channel(mesh, Target::kFace, "occupancy");
  EXPECT_EQ(occupancy.datatype, Datatype::kUint8);
  EXPECT_EQ(occupancy.info.unit, "cost");
  EXPECT_EQ(occupancy.info.no_data, std::optional<double>(255.0));
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occupancy", 1), 254.0);
  EXPECT_EQ(channel(mesh, Target::kFace, "cost").datatype, Datatype::kFloat32);
  EXPECT_TRUE(std::isnan(faceValue(mesh, "cost", 2)));
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 4), 4.5);
  EXPECT_EQ(channel(mesh, Target::kFace, "elevation").datatype, Datatype::kFloat64);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "elevation", 3), 1e300);
  const auto& log_odds = channel(mesh, Target::kFace, "occ_logodds");
  EXPECT_EQ(log_odds.datatype, Datatype::kInt16);
  EXPECT_EQ(log_odds.info.no_data, std::optional<double>(-32768.0));
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occ_logodds", 3), -2000.0);

  ASSERT_EQ(mesh.links.size(), 2u);
  EXPECT_EQ(mesh.links[0].from_face, 1u);
  EXPECT_EQ(mesh.links[0].to_face, 3u);
  EXPECT_EQ(mesh.links[0].kind, 2u);
  EXPECT_TRUE(mesh.links[0].bidirectional);
  EXPECT_DOUBLE_EQ(mesh.links[0].cost, 1.5);
  EXPECT_TRUE(std::isnan(mesh.links[1].cost));  // length -1: computed upstream

  bind(kNavMapLayerType, readTextFixture("navmap_layer_current.msg"));
  const auto update = parseObject<PJ::sdk::MeshMapUpdate>(readFixture("navmap_layer_current.cdr"));
  EXPECT_EQ(update.geometry_id, "");  // this recorded update leaves geometry_id unset (0)
  ASSERT_EQ(update.patches.size(), 1u);
  const auto& patch = update.patches[0];
  EXPECT_EQ(patch.channel.name, "occ_logodds");
  EXPECT_EQ(patch.channel.datatype, log_odds.datatype);
  EXPECT_EQ(patch.channel.info, log_odds.info);
  ASSERT_EQ(patch.indices.size(), 12u);  // NavCels 0, 2, 3
  EXPECT_DOUBLE_EQ(valueAt(patch.indices, Datatype::kUint32, 1, 2), 3.0);
  EXPECT_DOUBLE_EQ(valueAt(patch.channel.data, Datatype::kInt16, 1, 2), 7.0);
}

TEST_F(NavMapParserTest, DecodesARecordingOfTheFirstReleasedLayout) {
  bind(kNavMapType, readTextFixture("navmap_v1.msg"));
  const auto mesh = parseMap(readFixture("navmap_v1.cdr"));
  EXPECT_EQ(mesh.frame_id, "map");
  EXPECT_EQ(mesh.geometry_id, "");
  ASSERT_EQ(mesh.vertex_count, 7u);
  ASSERT_EQ(mesh.face_count, 5u);
  EXPECT_DOUBLE_EQ(position(mesh, 6, 2), 2.5);
  EXPECT_TRUE(mesh.links.empty());
  EXPECT_EQ(channel(mesh, Target::kFace, "occupancy").datatype, Datatype::kUint8);
  EXPECT_FALSE(channel(mesh, Target::kFace, "occupancy").info.no_data.has_value());
  EXPECT_DOUBLE_EQ(faceValue(mesh, "occupancy", 1), 254.0);
  EXPECT_EQ(channel(mesh, Target::kFace, "cost").datatype, Datatype::kFloat32);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "cost", 0), 0.5);
  EXPECT_EQ(channel(mesh, Target::kFace, "elevation").datatype, Datatype::kFloat64);
  EXPECT_DOUBLE_EQ(faceValue(mesh, "elevation", 0), 0.125);

  bind(kNavMapLayerType, readTextFixture("navmap_layer_v1.msg"));
  const auto update = parseObject<PJ::sdk::MeshMapUpdate>(readFixture("navmap_layer_v1.cdr"));
  ASSERT_EQ(update.patches.size(), 1u);
  EXPECT_EQ(update.patches[0].channel.name, "cost");
  EXPECT_EQ(update.patches[0].channel.datatype, Datatype::kFloat32);
  EXPECT_TRUE(update.patches[0].indices.empty());
  EXPECT_DOUBLE_EQ(valueAt(update.patches[0].channel.data, Datatype::kFloat32, 1, 4), 4.5);
}

}  // namespace
