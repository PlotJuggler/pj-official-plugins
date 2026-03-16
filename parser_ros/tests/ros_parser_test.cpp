#include "pj_plugins/host/message_parser_library.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <rosx_introspection/ros_parser.hpp>
#include <rosx_introspection/serializer.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"

#ifndef PJ_ROS_PARSER_PLUGIN_PATH
#error "PJ_ROS_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct RecordedField {
  std::string name;
  double value = 0.0;
  bool is_string = false;
  std::string string_value;
  PJ_primitive_type_t type = PJ_PRIMITIVE_TYPE_FLOAT64;
};

struct RecordedRow {
  int64_t timestamp = 0;
  std::vector<RecordedField> fields;
};

struct ParserWriteRecorder {
  std::vector<RecordedRow> rows;
  std::string last_error;

  static const char* getLastError(void* ctx) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    return self->last_error.empty() ? nullptr : self->last_error.c_str();
  }

  static bool ensureField(void*, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out_field) {
    *out_field = PJ_field_handle_t{{1}, 1};
    return true;
  }

  static bool appendRecord(void* ctx, int64_t timestamp, const PJ_named_field_value_t* fields, size_t field_count) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    RecordedRow row;
    row.timestamp = timestamp;
    for (size_t i = 0; i < field_count; ++i) {
      RecordedField f;
      f.name = std::string(fields[i].name.data, fields[i].name.size);
      f.type = fields[i].value.type;
      switch (fields[i].value.type) {
        case PJ_PRIMITIVE_TYPE_FLOAT64:
          f.value = fields[i].value.data.as_float64;
          break;
        case PJ_PRIMITIVE_TYPE_FLOAT32:
          f.value = static_cast<double>(fields[i].value.data.as_float32);
          break;
        case PJ_PRIMITIVE_TYPE_INT8:
          f.value = static_cast<double>(fields[i].value.data.as_int8);
          break;
        case PJ_PRIMITIVE_TYPE_INT16:
          f.value = static_cast<double>(fields[i].value.data.as_int16);
          break;
        case PJ_PRIMITIVE_TYPE_INT32:
          f.value = static_cast<double>(fields[i].value.data.as_int32);
          break;
        case PJ_PRIMITIVE_TYPE_INT64:
          f.value = static_cast<double>(fields[i].value.data.as_int64);
          break;
        case PJ_PRIMITIVE_TYPE_UINT8:
          f.value = static_cast<double>(fields[i].value.data.as_uint8);
          break;
        case PJ_PRIMITIVE_TYPE_UINT16:
          f.value = static_cast<double>(fields[i].value.data.as_uint16);
          break;
        case PJ_PRIMITIVE_TYPE_UINT32:
          f.value = static_cast<double>(fields[i].value.data.as_uint32);
          break;
        case PJ_PRIMITIVE_TYPE_UINT64:
          f.value = static_cast<double>(fields[i].value.data.as_uint64);
          break;
        case PJ_PRIMITIVE_TYPE_BOOL:
          f.value = fields[i].value.data.as_bool != 0 ? 1.0 : 0.0;
          break;
        case PJ_PRIMITIVE_TYPE_STRING:
          f.is_string = true;
          f.string_value =
              std::string(fields[i].value.data.as_string.data, fields[i].value.data.as_string.size);
          break;
        default:
          break;
      }
      row.fields.push_back(f);
    }
    self->rows.push_back(std::move(row));
    return true;
  }

  static bool appendBoundRecord(void*, int64_t, const PJ_bound_field_value_t*, size_t) { return true; }

  static bool appendArrowIpc(void*, PJ_bytes_view_t, PJ_string_view_t) { return true; }
};

PJ_parser_write_host_t makeWriteHost(ParserWriteRecorder* recorder) {
  static const PJ_parser_write_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_parser_write_host_vtable_t),
      .get_last_error = ParserWriteRecorder::getLastError,
      .ensure_field = ParserWriteRecorder::ensureField,
      .append_record = ParserWriteRecorder::appendRecord,
      .append_bound_record = ParserWriteRecorder::appendBoundRecord,
      .append_arrow_ipc = ParserWriteRecorder::appendArrowIpc,
  };
  return PJ_parser_write_host_t{.ctx = recorder, .vtable = &vtable};
}

struct RosParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_ROS_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(handle.bindWriteHost(makeWriteHost(&recorder)));
  }

  bool bindSchema(std::string_view type_name, const std::string& definition) {
    const auto* data = reinterpret_cast<const uint8_t*>(definition.data());
    return handle.bindSchema(type_name, {data, definition.size()});
  }

  bool parse(const std::vector<uint8_t>& payload, int64_t ts = 1000) {
    return handle.parse(ts, {payload.data(), payload.size()});
  }
};

// --- CDR serialization helpers ---

// Build a CDR-encoded buffer for a simple flat message using NanoCDR_Serializer.
// The message definition and the serialized fields must match.

std::vector<uint8_t> serializeCdr(const std::function<void(RosMsgParser::NanoCDR_Serializer&)>& fill) {
  RosMsgParser::NanoCDR_Serializer encoder;
  fill(encoder);
  return std::vector<uint8_t>(encoder.getBufferData(), encoder.getBufferData() + encoder.getBufferSize());
}

// --- ROS message definitions (text format) ---

// Simple scalar message: int32 + float64 + bool
static const char* kSimpleScalarDef =
    "int32 status\n"
    "float64 temperature\n"
    "bool active\n";

// Nested message: Header with stamp (sec/nanosec) + a float64 value
static const char* kNestedDef =
    "Header header\n"
    "float64 value\n"
    "================\n"
    "MSG: pkg/Header\n"
    "Stamp stamp\n"
    "string frame_id\n"
    "================\n"
    "MSG: pkg/Stamp\n"
    "int32 sec\n"
    "uint32 nanosec\n";

// String message: just a string field
static const char* kStringDef = "string data\n";

// Array message: fixed-size and variable-size arrays
static const char* kArrayDef =
    "float64[3] position\n"
    "int32 count\n";

// Variable-length array message
static const char* kVarArrayDef =
    "float64[] values\n"
    "int32 count\n";

// ---- Tests ----

TEST(RosParserTest, SimpleScalarMessage) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/ScalarMsg", kSimpleScalarDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(42)));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(23.5));
    enc.serialize(RosMsgParser::BOOL, RosMsgParser::Variant(static_cast<uint8_t>(1)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  bool found_status = false;
  bool found_temp = false;
  bool found_active = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "/status") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_INT32);
      EXPECT_DOUBLE_EQ(field.value, 42.0);
      found_status = true;
    } else if (field.name == "/temperature") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_FLOAT64);
      EXPECT_DOUBLE_EQ(field.value, 23.5);
      found_temp = true;
    } else if (field.name == "/active") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_BOOL);
      EXPECT_DOUBLE_EQ(field.value, 1.0);
      found_active = true;
    }
  }
  EXPECT_TRUE(found_status);
  EXPECT_TRUE(found_temp);
  EXPECT_TRUE(found_active);
}

TEST(RosParserTest, NestedMessage) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/Nested", kNestedDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    // header.stamp.sec (int32)
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(1234)));
    // header.stamp.nanosec (uint32)
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(567)));
    // header.frame_id (string)
    enc.serializeString("base_link");
    // value (float64)
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(3.14));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  bool found_sec = false;
  bool found_nanosec = false;
  bool found_frame_id = false;
  bool found_value = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "/header/stamp/sec") {
      EXPECT_DOUBLE_EQ(field.value, 1234.0);
      found_sec = true;
    } else if (field.name == "/header/stamp/nanosec") {
      EXPECT_DOUBLE_EQ(field.value, 567.0);
      found_nanosec = true;
    } else if (field.name == "/header/frame_id") {
      EXPECT_TRUE(field.is_string);
      EXPECT_EQ(field.string_value, "base_link");
      found_frame_id = true;
    } else if (field.name == "/value") {
      EXPECT_DOUBLE_EQ(field.value, 3.14);
      found_value = true;
    }
  }
  EXPECT_TRUE(found_sec);
  EXPECT_TRUE(found_nanosec);
  EXPECT_TRUE(found_frame_id);
  EXPECT_TRUE(found_value);
}

TEST(RosParserTest, StringField) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/StringMsg", kStringDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeString("hello world");
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "/data");
  EXPECT_TRUE(f.recorder.rows[0].fields[0].is_string);
  EXPECT_EQ(f.recorder.rows[0].fields[0].string_value, "hello world");
}

TEST(RosParserTest, Ros2TypeNameNormalization) {
  // "pkg/msg/Type" should be normalized to "pkg/Type" internally.
  // The parser should accept this and not throw.
  RosParserFixture f;
  f.setUp();

  const char* def = "int32 value\n";
  ASSERT_TRUE(f.bindSchema("pkg/msg/SimpleMsg", def));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(99)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "/value");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 99.0);
}

TEST(RosParserTest, FixedSizeArray) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/ArrayMsg", kArrayDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    // position[3] - fixed-size array, no length prefix
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(1.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(2.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(3.0));
    // count (int32)
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(3)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_GE(f.recorder.rows[0].fields.size(), 4u);

  // Check array elements
  bool found_pos0 = false;
  bool found_pos1 = false;
  bool found_pos2 = false;
  bool found_count = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "/position[0]") {
      EXPECT_DOUBLE_EQ(field.value, 1.0);
      found_pos0 = true;
    } else if (field.name == "/position[1]") {
      EXPECT_DOUBLE_EQ(field.value, 2.0);
      found_pos1 = true;
    } else if (field.name == "/position[2]") {
      EXPECT_DOUBLE_EQ(field.value, 3.0);
      found_pos2 = true;
    } else if (field.name == "/count") {
      EXPECT_DOUBLE_EQ(field.value, 3.0);
      found_count = true;
    }
  }
  EXPECT_TRUE(found_pos0);
  EXPECT_TRUE(found_pos1);
  EXPECT_TRUE(found_pos2);
  EXPECT_TRUE(found_count);
}

TEST(RosParserTest, VariableLengthArray) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/VarArrayMsg", kVarArrayDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    // Variable-length array: length prefix + elements
    enc.serializeUInt32(3);
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(10.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(20.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(30.0));
    // count (int32)
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(3)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_GE(f.recorder.rows[0].fields.size(), 4u);

  bool found_v0 = false;
  bool found_v1 = false;
  bool found_v2 = false;
  bool found_count = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "/values[0]") {
      EXPECT_DOUBLE_EQ(field.value, 10.0);
      found_v0 = true;
    } else if (field.name == "/values[1]") {
      EXPECT_DOUBLE_EQ(field.value, 20.0);
      found_v1 = true;
    } else if (field.name == "/values[2]") {
      EXPECT_DOUBLE_EQ(field.value, 30.0);
      found_v2 = true;
    } else if (field.name == "/count") {
      EXPECT_DOUBLE_EQ(field.value, 3.0);
      found_count = true;
    }
  }
  EXPECT_TRUE(found_v0);
  EXPECT_TRUE(found_v1);
  EXPECT_TRUE(found_v2);
  EXPECT_TRUE(found_count);
}

TEST(RosParserTest, InvalidSchemaFails) {
  RosParserFixture f;
  f.setUp();
  // An invalid definition should cause bindSchema to return an error.
  // Use a definition with an unknown type that should trigger a parse error.
  std::string bad_def = "unknown_type_xyz foo\n";
  EXPECT_FALSE(f.bindSchema("pkg/Bad", bad_def));
}

TEST(RosParserTest, ParseWithoutSchemaFails) {
  RosParserFixture f;
  f.setUp();
  // No bindSchema called — should fail.
  std::vector<uint8_t> dummy = {0, 1, 2, 3};
  EXPECT_FALSE(f.parse(dummy));
}

TEST(RosParserTest, ManifestContainsEncoding) {
  RosParserFixture f;
  f.setUp();
  EXPECT_NE(f.handle.manifest().find("\"encoding\":[\"ros2msg\""), std::string::npos);
}

TEST(RosParserTest, TimestampPreserved) {
  RosParserFixture f;
  f.setUp();
  const char* def = "int32 value\n";
  ASSERT_TRUE(f.bindSchema("pkg/Ts", def));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(1)));
  });

  ASSERT_TRUE(f.parse(payload, 99999));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].timestamp, 99999);
}

TEST(RosParserTest, NativeIntegerTypes) {
  // Verify that the parser emits native integer types, not just double.
  RosParserFixture f;
  f.setUp();

  const char* def =
      "int32 i32\n"
      "uint32 u32\n"
      "int64 i64\n"
      "uint64 u64\n"
      "int8 i8\n"
      "uint8 u8\n"
      "int16 i16\n"
      "uint16 u16\n"
      "float32 f32\n";
  ASSERT_TRUE(f.bindSchema("pkg/IntTypes", def));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(-42)));
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(100)));
    enc.serialize(RosMsgParser::INT64, RosMsgParser::Variant(static_cast<int64_t>(-9999)));
    enc.serialize(RosMsgParser::UINT64, RosMsgParser::Variant(static_cast<uint64_t>(12345)));
    enc.serialize(RosMsgParser::INT8, RosMsgParser::Variant(static_cast<int8_t>(-5)));
    enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(200)));
    enc.serialize(RosMsgParser::INT16, RosMsgParser::Variant(static_cast<int16_t>(-300)));
    enc.serialize(RosMsgParser::UINT16, RosMsgParser::Variant(static_cast<uint16_t>(400)));
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(static_cast<float>(1.5f)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "/i32") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_INT32);
      EXPECT_DOUBLE_EQ(field.value, -42.0);
    } else if (field.name == "/u32") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_UINT32);
      EXPECT_DOUBLE_EQ(field.value, 100.0);
    } else if (field.name == "/i64") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_INT64);
      EXPECT_DOUBLE_EQ(field.value, -9999.0);
    } else if (field.name == "/u64") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_UINT64);
      EXPECT_DOUBLE_EQ(field.value, 12345.0);
    } else if (field.name == "/i8") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_INT8);
    } else if (field.name == "/u8") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_UINT8);
    } else if (field.name == "/i16") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_INT16);
    } else if (field.name == "/u16") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_UINT16);
    } else if (field.name == "/f32") {
      EXPECT_EQ(field.type, PJ_PRIMITIVE_TYPE_FLOAT32);
    }
  }
}

TEST(RosParserTest, ArrayClampingConfig) {
  // Test that saveConfig/loadConfig round-trips max_array_size.
  RosParserFixture f;
  f.setUp();

  // Default config should have max_array_size = 500
  std::string cfg = f.handle.saveConfig();
  EXPECT_NE(cfg.find("\"max_array_size\""), std::string::npos);
  EXPECT_NE(cfg.find("500"), std::string::npos);

  // Load a custom config
  ASSERT_TRUE(f.handle.loadConfig(R"({"max_array_size":100})"));
  cfg = f.handle.saveConfig();
  EXPECT_NE(cfg.find("100"), std::string::npos);

  // Load empty/invalid JSON should use defaults (not fail)
  ASSERT_TRUE(f.handle.loadConfig("{}"));
}

// ---- Helper: find field by name ----

const RecordedField* findField(const RecordedRow& row, const std::string& name) {
  for (const auto& f : row.fields) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

// ---- Helper: serialize a ROS2 header (sec, nsec, frame_id) ----
void serializeHeader(RosMsgParser::NanoCDR_Serializer& enc, int32_t sec, uint32_t nsec,
                     const std::string& frame_id) {
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(sec));
  enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(nsec));
  enc.serializeString(frame_id);
}

// ---- Helper: serialize a quaternion (x,y,z,w) ----
void serializeQuaternion(RosMsgParser::NanoCDR_Serializer& enc, double x, double y, double z,
                         double w) {
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(x));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(y));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(z));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(w));
}

// ---- Helper: serialize a vector3 (x,y,z) ----
void serializeVector3(RosMsgParser::NanoCDR_Serializer& enc, double x, double y, double z) {
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(x));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(y));
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(z));
}

// ===== ROS message definitions for specialized types =====

static const char* kPoseDef =
    "Point position\n"
    "Quaternion orientation\n"
    "================\n"
    "MSG: geometry_msgs/Point\n"
    "float64 x\nfloat64 y\nfloat64 z\n"
    "================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

static const char* kPoseStampedDef =
    "std_msgs/Header header\n"
    "geometry_msgs/Pose pose\n"
    "================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\n"
    "string frame_id\n"
    "================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\nuint32 nanosec\n"
    "================\n"
    "MSG: geometry_msgs/Pose\n"
    "geometry_msgs/Point position\n"
    "geometry_msgs/Quaternion orientation\n"
    "================\n"
    "MSG: geometry_msgs/Point\n"
    "float64 x\nfloat64 y\nfloat64 z\n"
    "================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

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

static const char* kEmptyDef = "";

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

static const char* kDiagnosticArrayDef =
    "std_msgs/Header header\n"
    "diagnostic_msgs/DiagnosticStatus[] status\n"
    "================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\nstring frame_id\n"
    "================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\nuint32 nanosec\n"
    "================\n"
    "MSG: diagnostic_msgs/DiagnosticStatus\n"
    "uint8 level\nstring name\nstring message\nstring hardware_id\n"
    "diagnostic_msgs/KeyValue[] values\n"
    "================\n"
    "MSG: diagnostic_msgs/KeyValue\n"
    "string key\nstring value\n";

static const char* kTFMessageDef =
    "geometry_msgs/TransformStamped[] transforms\n"
    "================\n"
    "MSG: geometry_msgs/TransformStamped\n"
    "std_msgs/Header header\nstring child_frame_id\n"
    "geometry_msgs/Transform transform\n"
    "================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\nstring frame_id\n"
    "================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\nuint32 nanosec\n"
    "================\n"
    "MSG: geometry_msgs/Transform\n"
    "geometry_msgs/Vector3 translation\n"
    "geometry_msgs/Quaternion rotation\n"
    "================\n"
    "MSG: geometry_msgs/Vector3\n"
    "float64 x\nfloat64 y\nfloat64 z\n"
    "================\n"
    "MSG: geometry_msgs/Quaternion\n"
    "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

// ===== Specialization tests =====

TEST(RosParserTest, QuaternionRPY) {
  // Identity quaternion (0,0,0,1) → RPY all zeros.
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("geometry_msgs/Pose", kPoseDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeVector3(enc, 1.0, 2.0, 3.0);        // position
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);  // identity quaternion
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  auto* roll = findField(f.recorder.rows[0], "/orientation/roll");
  auto* pitch = findField(f.recorder.rows[0], "/orientation/pitch");
  auto* yaw = findField(f.recorder.rows[0], "/orientation/yaw");
  ASSERT_NE(roll, nullptr);
  ASSERT_NE(pitch, nullptr);
  ASSERT_NE(yaw, nullptr);
  EXPECT_NEAR(roll->value, 0.0, 1e-10);
  EXPECT_NEAR(pitch->value, 0.0, 1e-10);
  EXPECT_NEAR(yaw->value, 0.0, 1e-10);

  // Also check position fields.
  auto* px = findField(f.recorder.rows[0], "/position/x");
  ASSERT_NE(px, nullptr);
  EXPECT_DOUBLE_EQ(px->value, 1.0);
}

TEST(RosParserTest, PoseWithRPY) {
  // 90-degree rotation around Z: quaternion (0, 0, sin(45°), cos(45°))
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("geometry_msgs/Pose", kPoseDef));

  double angle = std::numbers::pi / 2.0;
  double qz = std::sin(angle / 2.0);
  double qw = std::cos(angle / 2.0);

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeVector3(enc, 10.0, 20.0, 30.0);
    serializeQuaternion(enc, 0.0, 0.0, qz, qw);
  });

  ASSERT_TRUE(f.parse(payload));
  auto* yaw = findField(f.recorder.rows[0], "/orientation/yaw");
  ASSERT_NE(yaw, nullptr);
  EXPECT_NEAR(yaw->value, std::numbers::pi / 2.0, 1e-10);

  auto* roll = findField(f.recorder.rows[0], "/orientation/roll");
  EXPECT_NEAR(roll->value, 0.0, 1e-10);

  // Check all 7 quaternion + RPY fields exist.
  EXPECT_NE(findField(f.recorder.rows[0], "/orientation/x"), nullptr);
  EXPECT_NE(findField(f.recorder.rows[0], "/orientation/y"), nullptr);
  EXPECT_NE(findField(f.recorder.rows[0], "/orientation/z"), nullptr);
  EXPECT_NE(findField(f.recorder.rows[0], "/orientation/w"), nullptr);
  EXPECT_NE(findField(f.recorder.rows[0], "/orientation/pitch"), nullptr);
}

TEST(RosParserTest, ImuRPY) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/Imu", kImuDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 100, 500000000, "imu_frame");
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);  // identity
    // orientation_covariance: 9 doubles
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i + 1)));
    }
    serializeVector3(enc, 0.1, 0.2, 0.3);  // angular_velocity
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
    }
    serializeVector3(enc, 9.8, 0.0, 0.0);  // linear_acceleration
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
    }
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  // RPY from identity quaternion.
  auto* roll = findField(f.recorder.rows[0], "/orientation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->value, 0.0, 1e-10);

  // Header stamp.
  auto* stamp = findField(f.recorder.rows[0], "/header/stamp");
  ASSERT_NE(stamp, nullptr);
  EXPECT_NEAR(stamp->value, 100.5, 1e-6);

  // Covariance upper-triangle: 3x3 → 6 entries.
  auto* cov00 = findField(f.recorder.rows[0], "/orientation_covariance/[0;0]");
  ASSERT_NE(cov00, nullptr);
  EXPECT_DOUBLE_EQ(cov00->value, 1.0);

  auto* cov01 = findField(f.recorder.rows[0], "/orientation_covariance/[0;1]");
  ASSERT_NE(cov01, nullptr);
  EXPECT_DOUBLE_EQ(cov01->value, 2.0);

  auto* cov22 = findField(f.recorder.rows[0], "/orientation_covariance/[2;2]");
  ASSERT_NE(cov22, nullptr);
  EXPECT_DOUBLE_EQ(cov22->value, 9.0);

  // Angular velocity.
  auto* ang_x = findField(f.recorder.rows[0], "/angular_velocity/x");
  ASSERT_NE(ang_x, nullptr);
  EXPECT_DOUBLE_EQ(ang_x->value, 0.1);

  // Linear acceleration.
  auto* lin_x = findField(f.recorder.rows[0], "/linear_acceleration/x");
  ASSERT_NE(lin_x, nullptr);
  EXPECT_DOUBLE_EQ(lin_x->value, 9.8);
}

TEST(RosParserTest, EmbeddedTimestamp) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.bindSchema("geometry_msgs/PoseStamped", kPoseStampedDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 42, 500000000, "base");
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
  });

  ASSERT_TRUE(f.parse(payload, /*host_ts=*/1000));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  // Embedded timestamp: 42 sec + 500000000 nsec = 42.5 sec = 42500000000 ns.
  EXPECT_EQ(f.recorder.rows[0].timestamp, 42500000000LL);
}

TEST(RosParserTest, EmbeddedTimestampDisabled) {
  RosParserFixture f;
  f.setUp();
  // Default: use_embedded_timestamp = false.
  ASSERT_TRUE(f.bindSchema("geometry_msgs/PoseStamped", kPoseStampedDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 42, 500000000, "base");
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
  });

  ASSERT_TRUE(f.parse(payload, /*host_ts=*/9999));
  EXPECT_EQ(f.recorder.rows[0].timestamp, 9999);
}

TEST(RosParserTest, CovarianceUpperTriangle6x6) {
  // Test via Odometry which has PoseWithCovariance (6×6) and TwistWithCovariance (6×6).
  // We just test that field naming is correct via a simpler path: Imu has 3×3 covariance.
  // The 6×6 case is tested implicitly through Odometry if needed.
  // Here we directly test the 3×3 from Imu: 6 upper triangle entries.
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/Imu", kImuDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 0, 0, "");
    serializeQuaternion(enc, 0, 0, 0, 1);
    // orientation_covariance: 9 values row-major
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(static_cast<double>(i)));
    }
    serializeVector3(enc, 0, 0, 0);
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
    }
    serializeVector3(enc, 0, 0, 0);
    for (int i = 0; i < 9; i++) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.0));
    }
  });

  ASSERT_TRUE(f.parse(payload));
  // 3×3 upper triangle: [0;0]=0, [0;1]=1, [0;2]=2, [1;1]=4, [1;2]=5, [2;2]=8
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[0;0]")->value, 0.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[0;1]")->value, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[0;2]")->value, 2.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[1;1]")->value, 4.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[1;2]")->value, 5.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[2;2]")->value, 8.0);
  // Lower triangle entries should NOT be present.
  EXPECT_EQ(findField(f.recorder.rows[0], "/orientation_covariance/[1;0]"), nullptr);
}

TEST(RosParserTest, Empty) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("std_msgs/Empty", kEmptyDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer&) {
    // Empty message: zero bytes.
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "/value");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 0.0);
}

TEST(RosParserTest, JointState) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/JointState", kJointStateDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 10, 0, "");
    // names: 3
    enc.serializeUInt32(3);
    enc.serializeString("shoulder");
    enc.serializeString("elbow");
    enc.serializeString("wrist");
    // positions: 3
    enc.serializeUInt32(3);
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(1.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(2.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(3.0));
    // velocities: 3
    enc.serializeUInt32(3);
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.1));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.2));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(0.3));
    // efforts: 3
    enc.serializeUInt32(3);
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(10.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(20.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(30.0));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/shoulder/position")->value, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/elbow/position")->value, 2.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/wrist/position")->value, 3.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/shoulder/velocity")->value, 0.1);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/wrist/effort")->value, 30.0);
}

TEST(RosParserTest, JointStatePartial) {
  // Names but no velocity/effort arrays.
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/JointState", kJointStateDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 10, 0, "");
    enc.serializeUInt32(2);
    enc.serializeString("j1");
    enc.serializeString("j2");
    // positions: 2
    enc.serializeUInt32(2);
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(1.0));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(2.0));
    // velocities: 0
    enc.serializeUInt32(0);
    // efforts: 0
    enc.serializeUInt32(0);
  });

  ASSERT_TRUE(f.parse(payload));
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/j1/position")->value, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows[0], "/j2/position")->value, 2.0);
  // No velocity or effort fields.
  EXPECT_EQ(findField(f.recorder.rows[0], "/j1/velocity"), nullptr);
  EXPECT_EQ(findField(f.recorder.rows[0], "/j1/effort"), nullptr);
}

TEST(RosParserTest, DiagnosticArray) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("diagnostic_msgs/DiagnosticArray", kDiagnosticArrayDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 1, 0, "");
    // 2 statuses
    enc.serializeUInt32(2);

    // Status 1: with hardware_id
    enc.serialize(RosMsgParser::BYTE, RosMsgParser::Variant(static_cast<uint8_t>(0)));  // level OK
    enc.serializeString("CPU Temperature");
    enc.serializeString("OK");
    enc.serializeString("cpu0");
    // 1 key-value pair
    enc.serializeUInt32(1);
    enc.serializeString("temperature");
    enc.serializeString("65.5");

    // Status 2: no hardware_id
    enc.serialize(RosMsgParser::BYTE, RosMsgParser::Variant(static_cast<uint8_t>(1)));  // level WARN
    enc.serializeString("Battery");
    enc.serializeString("Low");
    enc.serializeString("");
    enc.serializeUInt32(1);
    enc.serializeString("voltage");
    enc.serializeString("11.2");
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  // With hardware_id: /{hw_id}/{name}/{key}
  auto* temp = findField(f.recorder.rows[0], "/cpu0/CPU Temperature/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->value, 65.5);

  auto* level1 = findField(f.recorder.rows[0], "/cpu0/CPU Temperature/level");
  ASSERT_NE(level1, nullptr);
  EXPECT_DOUBLE_EQ(level1->value, 0.0);

  // Without hardware_id: /{name}/{key}
  auto* voltage = findField(f.recorder.rows[0], "/Battery/voltage");
  ASSERT_NE(voltage, nullptr);
  EXPECT_DOUBLE_EQ(voltage->value, 11.2);
}

TEST(RosParserTest, TFMessage) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("tf2_msgs/TFMessage", kTFMessageDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    // 2 transforms
    enc.serializeUInt32(2);

    // Transform 1: world → base_link
    serializeHeader(enc, 1, 0, "world");
    enc.serializeString("base_link");
    serializeVector3(enc, 1.0, 0.0, 0.0);          // translation
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);  // rotation (identity)

    // Transform 2: base_link → sensor
    serializeHeader(enc, 1, 0, "base_link");
    enc.serializeString("sensor");
    serializeVector3(enc, 0.0, 0.5, 0.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  auto* tx = findField(f.recorder.rows[0], "/world/base_link/translation/x");
  ASSERT_NE(tx, nullptr);
  EXPECT_DOUBLE_EQ(tx->value, 1.0);

  auto* ty = findField(f.recorder.rows[0], "/base_link/sensor/translation/y");
  ASSERT_NE(ty, nullptr);
  EXPECT_DOUBLE_EQ(ty->value, 0.5);

  // RPY fields from identity quaternion.
  auto* roll = findField(f.recorder.rows[0], "/world/base_link/rotation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->value, 0.0, 1e-10);
}

TEST(RosParserTest, ROS1Serialization) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"serialization":"ros1"})"));

  const char* def = "int32 value\nfloat64 temperature\n";
  ASSERT_TRUE(f.bindSchema("pkg/Simple", def));

  // ROS1 wire format: raw little-endian, no CDR encapsulation header.
  std::vector<uint8_t> payload;
  // int32 value = 42
  int32_t i32 = 42;
  auto* p = reinterpret_cast<const uint8_t*>(&i32);
  payload.insert(payload.end(), p, p + sizeof(i32));
  // float64 temperature = 23.5
  double f64 = 23.5;
  p = reinterpret_cast<const uint8_t*>(&f64);
  payload.insert(payload.end(), p, p + sizeof(f64));

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  auto* val = findField(f.recorder.rows[0], "/value");
  ASSERT_NE(val, nullptr);
  EXPECT_DOUBLE_EQ(val->value, 42.0);

  auto* temp = findField(f.recorder.rows[0], "/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->value, 23.5);
}

TEST(RosParserTest, ConfigRoundTrip) {
  RosParserFixture f;
  f.setUp();

  ASSERT_TRUE(f.handle.loadConfig(
      R"({"max_array_size":200,"use_embedded_timestamp":true,"serialization":"ros1","topic_name":"/test"})"));

  std::string cfg = f.handle.saveConfig();
  auto json = nlohmann::json::parse(cfg);
  EXPECT_EQ(json["max_array_size"], 200);
  EXPECT_EQ(json["use_embedded_timestamp"], true);
  EXPECT_EQ(json["serialization"], "ros1");
  EXPECT_EQ(json["topic_name"], "/test");
}

TEST(RosParserTest, GenericQuaternionRPY) {
  // Test that the generic path detects quaternion fields and adds RPY.
  // Use a custom message that contains a Quaternion but is NOT a known specialization.
  RosParserFixture f;
  f.setUp();

  const char* custom_def =
      "float64 value\n"
      "geometry_msgs/Quaternion rotation\n"
      "================\n"
      "MSG: geometry_msgs/Quaternion\n"
      "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

  ASSERT_TRUE(f.bindSchema("my_pkg/CustomMsg", custom_def));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(42.0));
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);  // identity
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  // Generic path should produce /rotation/x,y,z,w AND /rotation/roll,pitch,yaw.
  auto* roll = findField(f.recorder.rows[0], "/rotation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->value, 0.0, 1e-10);

  auto* w = findField(f.recorder.rows[0], "/rotation/w");
  ASSERT_NE(w, nullptr);
  EXPECT_DOUBLE_EQ(w->value, 1.0);
}

TEST(RosParserTest, GenericEmbeddedTimestamp) {
  // Test embedded timestamp extraction in the generic path for a non-specialized message
  // that has a Header.
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));

  const char* def =
      "std_msgs/Header header\n"
      "float64 data\n"
      "================\n"
      "MSG: std_msgs/Header\n"
      "builtin_interfaces/Time stamp\nstring frame_id\n"
      "================\n"
      "MSG: builtin_interfaces/Time\n"
      "int32 sec\nuint32 nanosec\n";

  ASSERT_TRUE(f.bindSchema("my_pkg/Stamped", def));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(100)));
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(0)));
    enc.serializeString("frame");
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(3.14));
  });

  ASSERT_TRUE(f.parse(payload, 1000));
  // Embedded timestamp: sec=100, nsec=0 → 100*1e9 = 100000000000
  EXPECT_EQ(f.recorder.rows[0].timestamp, 100000000000LL);
}

TEST(RosParserTest, TransformStampedSpecialization) {
  static const char* kTransformStampedDef =
      "std_msgs/Header header\n"
      "string child_frame_id\n"
      "geometry_msgs/Transform transform\n"
      "================\n"
      "MSG: std_msgs/Header\n"
      "builtin_interfaces/Time stamp\nstring frame_id\n"
      "================\n"
      "MSG: builtin_interfaces/Time\n"
      "int32 sec\nuint32 nanosec\n"
      "================\n"
      "MSG: geometry_msgs/Transform\n"
      "geometry_msgs/Vector3 translation\n"
      "geometry_msgs/Quaternion rotation\n"
      "================\n"
      "MSG: geometry_msgs/Vector3\n"
      "float64 x\nfloat64 y\nfloat64 z\n"
      "================\n"
      "MSG: geometry_msgs/Quaternion\n"
      "float64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("geometry_msgs/TransformStamped", kTransformStampedDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 5, 0, "world");
    enc.serializeString("robot");
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
  });

  ASSERT_TRUE(f.parse(payload));
  auto* child = findField(f.recorder.rows[0], "/child_frame_id");
  ASSERT_NE(child, nullptr);
  EXPECT_TRUE(child->is_string);
  EXPECT_EQ(child->string_value, "robot");

  auto* tx = findField(f.recorder.rows[0], "/transform/translation/x");
  ASSERT_NE(tx, nullptr);
  EXPECT_DOUBLE_EQ(tx->value, 1.0);
}

}  // namespace
