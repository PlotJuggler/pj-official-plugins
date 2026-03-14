#include "pj_plugins/host/message_parser_library.hpp"

#include <gtest/gtest.h>

#include <rosx_introspection/ros_parser.hpp>
#include <rosx_introspection/serializer.hpp>

#include <cstddef>
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
  EXPECT_NE(f.handle.manifest().find("\"encoding\":\"cdr\""), std::string::npos);
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

}  // namespace
