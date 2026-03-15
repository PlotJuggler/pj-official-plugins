#include "pj_plugins/host/message_parser_library.hpp"

#include <benchmark/benchmark.h>
#include <rosx_introspection/ros_parser.hpp>
#include <rosx_introspection/serializer.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"

#ifndef PJ_ROS_PARSER_PLUGIN_PATH
#error "PJ_ROS_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

// Lightweight write host that discards all output.
struct NullWriteHost {
  static const char* getLastError(void*) { return nullptr; }
  static bool ensureField(void*, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out) {
    *out = PJ_field_handle_t{{1}, 1};
    return true;
  }
  static bool appendRecord(void*, int64_t, const PJ_named_field_value_t*, size_t) { return true; }
  static bool appendBoundRecord(void*, int64_t, const PJ_bound_field_value_t*, size_t) { return true; }
  static bool appendArrowIpc(void*, PJ_bytes_view_t, PJ_string_view_t) { return true; }
};

PJ_parser_write_host_t makeNullHost() {
  static NullWriteHost host;
  static const PJ_parser_write_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_parser_write_host_vtable_t),
      .get_last_error = NullWriteHost::getLastError,
      .ensure_field = NullWriteHost::ensureField,
      .append_record = NullWriteHost::appendRecord,
      .append_bound_record = NullWriteHost::appendBoundRecord,
      .append_arrow_ipc = NullWriteHost::appendArrowIpc,
  };
  return {.ctx = &host, .vtable = &vtable};
}

// Serialize helpers
std::vector<uint8_t> serializeCdr(const std::function<void(RosMsgParser::NanoCDR_Serializer&)>& fill) {
  RosMsgParser::NanoCDR_Serializer encoder;
  fill(encoder);
  return {encoder.getBufferData(), encoder.getBufferData() + encoder.getBufferSize()};
}

void serializeHeader(RosMsgParser::NanoCDR_Serializer& enc, int32_t sec, uint32_t nsec,
                     const std::string& frame_id) {
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(sec));
  enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(nsec));
  enc.serializeString(frame_id);
}

void serializeVector3(RosMsgParser::NanoCDR_Serializer& enc, double x, double y, double z) {
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(x));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(y));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(z));
}

void serializeQuaternion(RosMsgParser::NanoCDR_Serializer& enc, double x, double y, double z,
                         double w) {
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(x));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(y));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(z));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(w));
}

// ----- Message definitions -----

static const char* kImuDef =
    "std_msgs/Header header\n"
    "geometry_msgs/Quaternion orientation\n"
    "float64[9] orientation_covariance\n"
    "geometry_msgs/Vector3 angular_velocity\n"
    "float64[9] angular_velocity_covariance\n"
    "geometry_msgs/Vector3 linear_acceleration\n"
    "float64[9] linear_acceleration_covariance\n"
    "================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\nstring frame_id\n"
    "================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\nuint32 nanosec\n"
    "================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n"
    "================\n"
    "MSG: geometry_msgs/Vector3\n"
    "float64 x\nfloat64 y\nfloat64 z\n";

static const char* kJointStateDef =
    "std_msgs/Header header\n"
    "string[] name\n"
    "float64[] position\n"
    "float64[] velocity\n"
    "float64[] effort\n"
    "================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\nstring frame_id\n"
    "================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\nuint32 nanosec\n";

static const char* kPoseDef =
    "Point position\n"
    "Quaternion orientation\n"
    "================\n"
    "MSG: geometry_msgs/Point\n"
    "float64 x\nfloat64 y\nfloat64 z\n"
    "================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

// ----- Pre-serialized payloads -----

std::vector<uint8_t> makeImuPayload() {
  return serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 100, 500000000, "imu_frame");
    serializeQuaternion(enc, 0.1, 0.2, 0.3, 0.9);
    for (int i = 0; i < 9; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i)));
    serializeVector3(enc, 0.01, 0.02, 0.03);
    for (int i = 0; i < 9; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
    serializeVector3(enc, 9.8, 0.0, 0.0);
    for (int i = 0; i < 9; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
  });
}

std::vector<uint8_t> makeJointStatePayload() {
  return serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 10, 0, "");
    enc.serializeUInt32(6);
    for (auto name : {"shoulder_pan", "shoulder_lift", "elbow", "wrist_1", "wrist_2", "wrist_3"})
      enc.serializeString(name);
    enc.serializeUInt32(6);
    for (int i = 0; i < 6; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i) * 0.1));
    enc.serializeUInt32(6);
    for (int i = 0; i < 6; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i) * 0.01));
    enc.serializeUInt32(6);
    for (int i = 0; i < 6; i++)
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i) * 10.0));
  });
}

std::vector<uint8_t> makePosePayload() {
  return serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.7071, 0.7071);
  });
}

// ----- Benchmark fixture -----

struct ParserBench {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};

  bool setup(const char* type_name, const char* def) {
    auto lib = PJ::MessageParserLibrary::load(PJ_ROS_PARSER_PLUGIN_PATH);
    if (!lib) return false;
    library = std::move(*lib);
    handle = library.createHandle();
    if (!handle.valid()) return false;
    if (!handle.bindWriteHost(makeNullHost())) return false;
    auto* data = reinterpret_cast<const uint8_t*>(def);
    return handle.bindSchema(type_name, {data, std::strlen(def)});
  }
};

// ----- Benchmarks -----

void BM_ParseImu(benchmark::State& state) {
  ParserBench bench;
  if (!bench.setup("sensor_msgs/Imu", kImuDef)) {
    state.SkipWithError("setup failed");
    return;
  }
  auto payload = makeImuPayload();
  int64_t ts = 1000;
  for (auto _ : state) {
    bench.handle.parse(ts++, {payload.data(), payload.size()});
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseImu);

void BM_ParseJointState(benchmark::State& state) {
  ParserBench bench;
  if (!bench.setup("sensor_msgs/JointState", kJointStateDef)) {
    state.SkipWithError("setup failed");
    return;
  }
  auto payload = makeJointStatePayload();
  int64_t ts = 1000;
  for (auto _ : state) {
    bench.handle.parse(ts++, {payload.data(), payload.size()});
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseJointState);

void BM_ParsePose(benchmark::State& state) {
  ParserBench bench;
  if (!bench.setup("geometry_msgs/Pose", kPoseDef)) {
    state.SkipWithError("setup failed");
    return;
  }
  auto payload = makePosePayload();
  int64_t ts = 1000;
  for (auto _ : state) {
    bench.handle.parse(ts++, {payload.data(), payload.size()});
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParsePose);

}  // namespace

BENCHMARK_MAIN();
