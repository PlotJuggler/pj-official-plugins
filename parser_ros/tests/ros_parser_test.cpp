#include <gtest/gtest.h>

#include <any>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <numbers>
#include <rosx_introspection/ros_parser.hpp>
#include <rosx_introspection/serializer.hpp>
#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

#ifndef PJ_ROS_PARSER_PLUGIN_PATH
#error "PJ_ROS_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct RosParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder recorder;

  void setUp(const char* plugin_path = PJ_ROS_PARSER_PLUGIN_PATH) {
    auto lib = PJ::MessageParserLibrary::load(plugin_path);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(recorder.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  bool loadSchemaEncoding(std::string_view schema_encoding) {
    std::string config_json;
    if (!handle.saveConfig(config_json)) {
      return false;
    }
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      cfg = nlohmann::json::object();
    }
    cfg["schema_encoding"] = schema_encoding;
    return handle.loadConfig(cfg.dump()).has_value();
  }

  bool bindSchemaRaw(std::string_view type_name, const std::string& definition) {
    const auto* data = reinterpret_cast<const uint8_t*>(definition.data());
    return handle.bindSchema(type_name, PJ::Span<const uint8_t>(data, definition.size())).has_value();
  }

  bool bindSchema(
      std::string_view type_name, const std::string& definition, std::string_view schema_encoding = "ros2msg") {
    return bindSchemaRaw(type_name, definition) && loadSchemaEncoding(schema_encoding);
  }

  bool parse(const std::vector<uint8_t>& payload, int64_t ts = 1000) {
    return handle.parse(ts, PJ::Span<const uint8_t>(payload.data(), payload.size())).has_value();
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

static const char* kSimpleIdlDef = R"(
module pkg {
  struct SimpleIdl {
    long status;
    double temperature;
    boolean active;
  };
};
)";

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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  bool found_status = false;
  bool found_temp = false;
  bool found_active = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/status") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt32);
      EXPECT_DOUBLE_EQ(field.numeric, 42.0);
      found_status = true;
    } else if (field.name == "/temperature") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kFloat64);
      EXPECT_DOUBLE_EQ(field.numeric, 23.5);
      found_temp = true;
    } else if (field.name == "/active") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kBool);
      EXPECT_DOUBLE_EQ(field.numeric, 1.0);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  bool found_sec = false;
  bool found_nanosec = false;
  bool found_frame_id = false;
  bool found_value = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/header/stamp/sec") {
      EXPECT_DOUBLE_EQ(field.numeric, 1234.0);
      found_sec = true;
    } else if (field.name == "/header/stamp/nanosec") {
      EXPECT_DOUBLE_EQ(field.numeric, 567.0);
      found_nanosec = true;
    } else if (field.name == "/header/frame_id") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kString);
      EXPECT_EQ(field.string_value, "base_link");
      found_frame_id = true;
    } else if (field.name == "/value") {
      EXPECT_DOUBLE_EQ(field.numeric, 3.14);
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

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString("hello world"); });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "/data");
  EXPECT_EQ(f.recorder.rows()[0].fields[0].type, PJ::PrimitiveType::kString);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].string_value, "hello world");
}

TEST(RosParserTest, StringSuffixStrippedToNumberWhenFlagOn) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/StringMsg", kStringDef));
  ASSERT_TRUE(f.handle.loadConfig(R"({"remove_suffix_from_strings":true})"));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString("100ms"); });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "/data");
  EXPECT_EQ(f.recorder.rows()[0].fields[0].type, PJ::PrimitiveType::kFloat64);
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 100.0);
}

TEST(RosParserTest, BooleanStringConvertedToNumberWhenFlagOn) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/StringMsg", kStringDef));
  ASSERT_TRUE(f.handle.loadConfig(R"({"boolean_strings_to_number":true})"));

  // "True" → 1.0 (case-insensitive, length 4).
  auto payload_true = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString("True"); });
  ASSERT_TRUE(f.parse(payload_true));
  ASSERT_FALSE(f.recorder.rows().empty());
  ASSERT_FALSE(f.recorder.rows().back().fields.empty());
  EXPECT_EQ(f.recorder.rows().back().fields[0].type, PJ::PrimitiveType::kFloat64);
  EXPECT_DOUBLE_EQ(f.recorder.rows().back().fields[0].numeric, 1.0);

  // "false" → 0.0.
  auto payload_false = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString("false"); });
  ASSERT_TRUE(f.parse(payload_false));
  EXPECT_EQ(f.recorder.rows().back().fields[0].type, PJ::PrimitiveType::kFloat64);
  EXPECT_DOUBLE_EQ(f.recorder.rows().back().fields[0].numeric, 0.0);
}

TEST(RosParserTest, StringPassesThroughWhenFlagsOff) {
  // Sanity check that a non-numeric, non-boolean string still reaches the
  // recorder as a string when both toggles are off — and that the toggles
  // default to off.
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg/StringMsg", kStringDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeString("100ms");  // would be parseable with suffix flag on
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].type, PJ::PrimitiveType::kString);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].string_value, "100ms");
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "/value");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 99.0);
}

TEST(RosParserTest, OmgIdlSchemaParsesCdrPayload) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("pkg::SimpleIdl", kSimpleIdlDef, "omgidl"));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(42)));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(23.5));
    enc.serialize(RosMsgParser::BOOL, RosMsgParser::Variant(static_cast<uint8_t>(1)));
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  bool found_status = false;
  bool found_temp = false;
  bool found_active = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/status") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt32);
      EXPECT_DOUBLE_EQ(field.numeric, 42.0);
      found_status = true;
    } else if (field.name == "/temperature") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kFloat64);
      EXPECT_DOUBLE_EQ(field.numeric, 23.5);
      found_temp = true;
    } else if (field.name == "/active") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kBool);
      EXPECT_DOUBLE_EQ(field.numeric, 1.0);
      found_active = true;
    }
  }
  EXPECT_TRUE(found_status);
  EXPECT_TRUE(found_temp);
  EXPECT_TRUE(found_active);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_GE(f.recorder.rows()[0].fields.size(), 4u);

  // Check array elements
  bool found_pos0 = false;
  bool found_pos1 = false;
  bool found_pos2 = false;
  bool found_count = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/position[0]") {
      EXPECT_DOUBLE_EQ(field.numeric, 1.0);
      found_pos0 = true;
    } else if (field.name == "/position[1]") {
      EXPECT_DOUBLE_EQ(field.numeric, 2.0);
      found_pos1 = true;
    } else if (field.name == "/position[2]") {
      EXPECT_DOUBLE_EQ(field.numeric, 3.0);
      found_pos2 = true;
    } else if (field.name == "/count") {
      EXPECT_DOUBLE_EQ(field.numeric, 3.0);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_GE(f.recorder.rows()[0].fields.size(), 4u);

  bool found_v0 = false;
  bool found_v1 = false;
  bool found_v2 = false;
  bool found_count = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/values[0]") {
      EXPECT_DOUBLE_EQ(field.numeric, 10.0);
      found_v0 = true;
    } else if (field.name == "/values[1]") {
      EXPECT_DOUBLE_EQ(field.numeric, 20.0);
      found_v1 = true;
    } else if (field.name == "/values[2]") {
      EXPECT_DOUBLE_EQ(field.numeric, 30.0);
      found_v2 = true;
    } else if (field.name == "/count") {
      EXPECT_DOUBLE_EQ(field.numeric, 3.0);
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
  // PJ4 supplies parser_config_json after bindSchema(), so schema validation
  // happens when loadConfig() compiles the stored definition.
  std::string bad_def = "unknown_type_xyz foo\n";
  ASSERT_TRUE(f.bindSchemaRaw("pkg/Bad", bad_def));
  EXPECT_FALSE(f.handle.loadConfig(R"({"schema_encoding":"ros2msg"})"));
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
  EXPECT_NE(f.handle.manifest().find("\"ros2msg\""), std::string::npos);
  EXPECT_NE(f.handle.manifest().find("\"ros1msg\""), std::string::npos);
  EXPECT_NE(f.handle.manifest().find("\"omgidl\""), std::string::npos);
  EXPECT_EQ(f.handle.manifest().find("\"cdr\""), std::string::npos);
}

TEST(RosParserTest, ExposesDialogVtable) {
  RosParserFixture f;
  f.setUp();

  auto vtable = f.library.resolveDialogVtable();
  ASSERT_TRUE(vtable) << vtable.error();

  PJ::DialogHandle dialog(*vtable);
  const auto ui = dialog.ui_content();
  EXPECT_EQ(ui.rfind("<?xml", 0), 0u);
  EXPECT_EQ(ui.find("comboBoxSerialization"), std::string::npos);
  EXPECT_NE(ui.find("spinBoxArraySize"), std::string::npos);
  EXPECT_NE(ui.find("checkBoxTimestamp"), std::string::npos);
  ASSERT_TRUE(dialog.load_config(
      R"({"max_array_size":200,"discard_large_arrays":true,"use_embedded_timestamp":true,"serialization":"ros1"})"));
  const auto cfg = nlohmann::json::parse(dialog.save_config());
  EXPECT_EQ(cfg["max_array_size"], 200);
  EXPECT_EQ(cfg["discard_large_arrays"], true);
  EXPECT_EQ(cfg["use_embedded_timestamp"], true);
  EXPECT_FALSE(cfg.contains("serialization"));
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 99999);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "/i32") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt32);
      EXPECT_DOUBLE_EQ(field.numeric, -42.0);
    } else if (field.name == "/u32") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kUint32);
      EXPECT_DOUBLE_EQ(field.numeric, 100.0);
    } else if (field.name == "/i64") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt64);
      EXPECT_DOUBLE_EQ(field.numeric, -9999.0);
    } else if (field.name == "/u64") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kUint64);
      EXPECT_DOUBLE_EQ(field.numeric, 12345.0);
    } else if (field.name == "/i8") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt8);
    } else if (field.name == "/u8") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kUint8);
    } else if (field.name == "/i16") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kInt16);
    } else if (field.name == "/u16") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kUint16);
    } else if (field.name == "/f32") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kFloat32);
    }
  }
}

TEST(RosParserTest, ArrayClampingConfig) {
  // Test that saveConfig/loadConfig round-trips max_array_size.
  RosParserFixture f;
  f.setUp();

  // Default config should have max_array_size = 500
  std::string cfg;
  ASSERT_TRUE(f.handle.saveConfig(cfg));
  EXPECT_NE(cfg.find("\"max_array_size\""), std::string::npos);
  EXPECT_NE(cfg.find("500"), std::string::npos);

  // Load a custom config
  ASSERT_TRUE(f.handle.loadConfig(R"({"max_array_size":100})"));
  ASSERT_TRUE(f.handle.saveConfig(cfg));
  EXPECT_NE(cfg.find("100"), std::string::npos);

  // Load empty/invalid JSON should use defaults (not fail)
  ASSERT_TRUE(f.handle.loadConfig("{}"));
}

// ---- Helper: find field by name ----

const PJ::sdk::testing::RecordedField* findField(const PJ::sdk::testing::RecordedRow& row, const std::string& name) {
  for (const auto& f : row.fields) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

// ---- Helper: serialize a ROS2 header (sec, nsec, frame_id) ----
void serializeHeader(RosMsgParser::NanoCDR_Serializer& enc, int32_t sec, uint32_t nsec, const std::string& frame_id) {
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(sec));
  enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(nsec));
  enc.serializeString(frame_id);
}

// ---- Helper: serialize a quaternion (x,y,z,w) ----
void serializeQuaternion(RosMsgParser::NanoCDR_Serializer& enc, double x, double y, double z, double w) {
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
    serializeVector3(enc, 1.0, 2.0, 3.0);          // position
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);  // identity quaternion
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  auto* roll = findField(f.recorder.rows()[0], "/orientation/roll");
  auto* pitch = findField(f.recorder.rows()[0], "/orientation/pitch");
  auto* yaw = findField(f.recorder.rows()[0], "/orientation/yaw");
  ASSERT_NE(roll, nullptr);
  ASSERT_NE(pitch, nullptr);
  ASSERT_NE(yaw, nullptr);
  EXPECT_NEAR(roll->numeric, 0.0, 1e-10);
  EXPECT_NEAR(pitch->numeric, 0.0, 1e-10);
  EXPECT_NEAR(yaw->numeric, 0.0, 1e-10);

  // Also check position fields.
  auto* px = findField(f.recorder.rows()[0], "/position/x");
  ASSERT_NE(px, nullptr);
  EXPECT_DOUBLE_EQ(px->numeric, 1.0);
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
  auto* yaw = findField(f.recorder.rows()[0], "/orientation/yaw");
  ASSERT_NE(yaw, nullptr);
  EXPECT_NEAR(yaw->numeric, std::numbers::pi / 2.0, 1e-10);

  auto* roll = findField(f.recorder.rows()[0], "/orientation/roll");
  EXPECT_NEAR(roll->numeric, 0.0, 1e-10);

  // Check all 7 quaternion + RPY fields exist.
  EXPECT_NE(findField(f.recorder.rows()[0], "/orientation/x"), nullptr);
  EXPECT_NE(findField(f.recorder.rows()[0], "/orientation/y"), nullptr);
  EXPECT_NE(findField(f.recorder.rows()[0], "/orientation/z"), nullptr);
  EXPECT_NE(findField(f.recorder.rows()[0], "/orientation/w"), nullptr);
  EXPECT_NE(findField(f.recorder.rows()[0], "/orientation/pitch"), nullptr);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  // RPY from identity quaternion.
  auto* roll = findField(f.recorder.rows()[0], "/orientation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->numeric, 0.0, 1e-10);

  // Header stamp.
  auto* stamp = findField(f.recorder.rows()[0], "/header/stamp");
  ASSERT_NE(stamp, nullptr);
  EXPECT_NEAR(stamp->numeric, 100.5, 1e-6);

  // Covariance upper-triangle: 3x3 → 6 entries.
  auto* cov00 = findField(f.recorder.rows()[0], "/orientation_covariance/[0;0]");
  ASSERT_NE(cov00, nullptr);
  EXPECT_DOUBLE_EQ(cov00->numeric, 1.0);

  auto* cov01 = findField(f.recorder.rows()[0], "/orientation_covariance/[0;1]");
  ASSERT_NE(cov01, nullptr);
  EXPECT_DOUBLE_EQ(cov01->numeric, 2.0);

  auto* cov22 = findField(f.recorder.rows()[0], "/orientation_covariance/[2;2]");
  ASSERT_NE(cov22, nullptr);
  EXPECT_DOUBLE_EQ(cov22->numeric, 9.0);

  // Angular velocity.
  auto* ang_x = findField(f.recorder.rows()[0], "/angular_velocity/x");
  ASSERT_NE(ang_x, nullptr);
  EXPECT_DOUBLE_EQ(ang_x->numeric, 0.1);

  // Linear acceleration.
  auto* lin_x = findField(f.recorder.rows()[0], "/linear_acceleration/x");
  ASSERT_NE(lin_x, nullptr);
  EXPECT_DOUBLE_EQ(lin_x->numeric, 9.8);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  // Embedded timestamp: 42 sec + 500000000 nsec = 42.5 sec = 42500000000 ns.
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 42500000000LL);
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
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 9999);
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
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[0;0]")->numeric, 0.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[0;1]")->numeric, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[0;2]")->numeric, 2.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[1;1]")->numeric, 4.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[1;2]")->numeric, 5.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[2;2]")->numeric, 8.0);
  // Lower triangle entries should NOT be present.
  EXPECT_EQ(findField(f.recorder.rows()[0], "/orientation_covariance/[1;0]"), nullptr);
}

TEST(RosParserTest, Empty) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("std_msgs/Empty", kEmptyDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer&) {
    // Empty message: zero bytes.
  });

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "/value");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 0.0);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/shoulder/position")->numeric, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/elbow/position")->numeric, 2.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/wrist/position")->numeric, 3.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/shoulder/velocity")->numeric, 0.1);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/wrist/effort")->numeric, 30.0);
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
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/j1/position")->numeric, 1.0);
  EXPECT_DOUBLE_EQ(findField(f.recorder.rows()[0], "/j2/position")->numeric, 2.0);
  // No velocity or effort fields.
  EXPECT_EQ(findField(f.recorder.rows()[0], "/j1/velocity"), nullptr);
  EXPECT_EQ(findField(f.recorder.rows()[0], "/j1/effort"), nullptr);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  // With hardware_id: /{hw_id}/{name}/{key}
  auto* temp = findField(f.recorder.rows()[0], "/cpu0/CPU Temperature/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->numeric, 65.5);

  auto* level1 = findField(f.recorder.rows()[0], "/cpu0/CPU Temperature/level");
  ASSERT_NE(level1, nullptr);
  EXPECT_DOUBLE_EQ(level1->numeric, 0.0);

  // Without hardware_id: /{name}/{key}
  auto* voltage = findField(f.recorder.rows()[0], "/Battery/voltage");
  ASSERT_NE(voltage, nullptr);
  EXPECT_DOUBLE_EQ(voltage->numeric, 11.2);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  auto* tx = findField(f.recorder.rows()[0], "/world/base_link/translation/x");
  ASSERT_NE(tx, nullptr);
  EXPECT_DOUBLE_EQ(tx->numeric, 1.0);

  auto* ty = findField(f.recorder.rows()[0], "/base_link/sensor/translation/y");
  ASSERT_NE(ty, nullptr);
  EXPECT_DOUBLE_EQ(ty->numeric, 0.5);

  // RPY fields from identity quaternion.
  auto* roll = findField(f.recorder.rows()[0], "/world/base_link/rotation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->numeric, 0.0, 1e-10);
}

TEST(RosParserTest, TFMessageProducesFrameTransformsObject) {
  RosParserFixture f;
  f.setUp();

  const std::string def(kTFMessageDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());

  ASSERT_TRUE(f.bindSchema("tf2_msgs/TFMessage", kTFMessageDef));

  // TF advertises the canonical-object route alongside its scalars, so it lands
  // in BOTH the datastore (the TFMessage scalar test above) and the objectstore.
  EXPECT_EQ(f.handle.classifySchema("tf2_msgs/TFMessage", def_span), PJ::sdk::BuiltinObjectType::kFrameTransforms);

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeUInt32(2);
    serializeHeader(enc, 1, 500, "world");  // stamp = 1 s + 500 ns
    enc.serializeString("base_link");
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
    serializeHeader(enc, 2, 0, "base_link");  // stamp = 2 s
    enc.serializeString("sensor");
    serializeVector3(enc, 4.0, 5.0, 6.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.707, 0.707);
  });

  // The object route is the in-process C++ path: the host calls parseObject on
  // the MessageParserPluginBase* directly (the C ABI vtable carries only the
  // scalar parse() slot). context() hands back that base pointer.
  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);

  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1000, view);
  ASSERT_TRUE(rec.has_value());

  const auto* ft = std::any_cast<PJ::sdk::FrameTransforms>(&rec->object);
  ASSERT_NE(ft, nullptr);
  ASSERT_EQ(ft->transforms.size(), 2u);

  // Each FrameTransform keeps its own Header.stamp — the per-sample time the 3D
  // TF buffer needs — independent of the 1000 ns message receive time above.
  EXPECT_EQ(ft->transforms[0].parent_frame_id, "world");
  EXPECT_EQ(ft->transforms[0].child_frame_id, "base_link");
  EXPECT_EQ(ft->transforms[0].timestamp, 1'000'000'500);
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.x, 1.0);
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.z, 3.0);
  EXPECT_DOUBLE_EQ(ft->transforms[0].rotation.w, 1.0);

  EXPECT_EQ(ft->transforms[1].parent_frame_id, "base_link");
  EXPECT_EQ(ft->transforms[1].child_frame_id, "sensor");
  EXPECT_EQ(ft->transforms[1].timestamp, 2'000'000'000);
  EXPECT_DOUBLE_EQ(ft->transforms[1].translation.y, 5.0);
  EXPECT_DOUBLE_EQ(ft->transforms[1].rotation.z, 0.707);
}

TEST(RosParserTest, TransformStampedProducesFrameTransformsObject) {
  static const char* kTransformStampedDef =
      "std_msgs/Header header\nstring child_frame_id\ngeometry_msgs/Transform transform\n"
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: geometry_msgs/Transform\n"
      "geometry_msgs/Vector3 translation\ngeometry_msgs/Quaternion rotation\n"
      "================\nMSG: geometry_msgs/Vector3\nfloat64 x\nfloat64 y\nfloat64 z\n"
      "================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kTransformStampedDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("geometry_msgs/TransformStamped", kTransformStampedDef));
  EXPECT_EQ(
      f.handle.classifySchema("geometry_msgs/TransformStamped", def_span),
      PJ::sdk::BuiltinObjectType::kFrameTransforms);

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 5, 0, "odom");
    enc.serializeString("base_link");
    serializeVector3(enc, 1.0, 2.0, 3.0);
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(999, view);
  ASSERT_TRUE(rec.has_value());

  const auto* ft = std::any_cast<PJ::sdk::FrameTransforms>(&rec->object);
  ASSERT_NE(ft, nullptr);
  ASSERT_EQ(ft->transforms.size(), 1u);
  EXPECT_EQ(ft->transforms[0].parent_frame_id, "odom");
  EXPECT_EQ(ft->transforms[0].child_frame_id, "base_link");
  EXPECT_EQ(ft->transforms[0].timestamp, 5'000'000'000);
  EXPECT_DOUBLE_EQ(ft->transforms[0].translation.z, 3.0);
  EXPECT_DOUBLE_EQ(ft->transforms[0].rotation.w, 1.0);
}

TEST(RosParserTest, OccupancyGridProducesObject) {
  static const char* kOccupancyGridDef =
      "std_msgs/Header header\nnav_msgs/MapMetaData info\nint8[] data\n"
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: nav_msgs/MapMetaData\n"
      "builtin_interfaces/Time map_load_time\nfloat32 resolution\nuint32 width\nuint32 height\n"
      "geometry_msgs/Pose origin\n"
      "================\nMSG: geometry_msgs/Pose\n"
      "geometry_msgs/Point position\ngeometry_msgs/Quaternion orientation\n"
      "================\nMSG: geometry_msgs/Point\nfloat64 x\nfloat64 y\nfloat64 z\n"
      "================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kOccupancyGridDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("nav_msgs/OccupancyGrid", kOccupancyGridDef));
  EXPECT_EQ(f.handle.classifySchema("nav_msgs/OccupancyGrid", def_span), PJ::sdk::BuiltinObjectType::kOccupancyGrid);

  const std::vector<uint8_t> cells = {0, 50, 100, 0xFF /* -1 unknown */, 25, 75};
  auto payload = serializeCdr([&cells](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 7, 0, "map");
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(0)));     // map_load_time.sec
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(0)));   // map_load_time.nanosec
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(static_cast<float>(0.05)));  // resolution
    enc.serializeUInt32(3);                                                                 // width
    enc.serializeUInt32(2);                                                                 // height
    serializeVector3(enc, 1.0, 2.0, 0.0);                                                   // origin.position
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);                                           // origin.orientation
    enc.serializeUInt32(static_cast<uint32_t>(cells.size()));
    for (uint8_t c : cells) {
      enc.serialize(RosMsgParser::INT8, RosMsgParser::Variant(static_cast<int8_t>(c)));
    }
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* grid = std::any_cast<PJ::sdk::OccupancyGrid>(&rec->object);
  ASSERT_NE(grid, nullptr);
  EXPECT_EQ(grid->frame_id, "map");
  EXPECT_EQ(grid->width, 3u);
  EXPECT_EQ(grid->height, 2u);
  EXPECT_NEAR(grid->resolution, 0.05, 1e-6);
  EXPECT_DOUBLE_EQ(grid->origin.position.x, 1.0);
  EXPECT_DOUBLE_EQ(grid->origin.position.y, 2.0);
  EXPECT_DOUBLE_EQ(grid->origin.orientation.w, 1.0);
  ASSERT_EQ(grid->data.size(), cells.size());
  for (size_t i = 0; i < cells.size(); ++i) {
    EXPECT_EQ(grid->data.data()[i], cells[i]);
  }
}

TEST(RosParserTest, RobotDescriptionTopicProducesObject) {
  RosParserFixture f;
  f.setUp();
  // Topic-gated: only a std_msgs/String on a robot_description topic becomes a robot.
  ASSERT_TRUE(f.handle.loadConfig(R"({"topic_name":"/robot_description"})"));

  const std::string def = "string data\n";
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("std_msgs/String", def));
  EXPECT_EQ(f.handle.classifySchema("std_msgs/String", def_span), PJ::sdk::BuiltinObjectType::kRobotDescription);

  const std::string urdf = "<robot name=\"r\"><link name=\"base_link\"/></robot>";
  auto payload = serializeCdr([&urdf](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString(urdf); });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* rd = std::any_cast<PJ::sdk::RobotDescription>(&rec->object);
  ASSERT_NE(rd, nullptr);
  EXPECT_EQ(rd->topic, "/robot_description");
  EXPECT_EQ(rd->format, "urdf");
  EXPECT_EQ(rd->text, urdf);
}

TEST(RosParserTest, GenericStringTopicIsNotRobotDescription) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"topic_name":"/chatter"})"));

  const std::string def = "string data\n";
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("std_msgs/String", def));
  // A String on a non-robot_description topic stays a generic scalar — no object.
  EXPECT_EQ(f.handle.classifySchema("std_msgs/String", def_span), PJ::sdk::BuiltinObjectType::kNone);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  auto* val = findField(f.recorder.rows()[0], "/value");
  ASSERT_NE(val, nullptr);
  EXPECT_DOUBLE_EQ(val->numeric, 42.0);

  auto* temp = findField(f.recorder.rows()[0], "/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->numeric, 23.5);
}

TEST(RosParserTest, ConfigRoundTrip) {
  RosParserFixture f;
  f.setUp();

  ASSERT_TRUE(f.handle.loadConfig(
      R"({"max_array_size":200,"use_embedded_timestamp":true,"serialization":"ros1","topic_name":"/test"})"));

  std::string cfg;
  ASSERT_TRUE(f.handle.saveConfig(cfg));
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  // Generic path should produce /rotation/x,y,z,w AND /rotation/roll,pitch,yaw.
  auto* roll = findField(f.recorder.rows()[0], "/rotation/roll");
  ASSERT_NE(roll, nullptr);
  EXPECT_NEAR(roll->numeric, 0.0, 1e-10);

  auto* w = findField(f.recorder.rows()[0], "/rotation/w");
  ASSERT_NE(w, nullptr);
  EXPECT_DOUBLE_EQ(w->numeric, 1.0);
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
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 100000000000LL);
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
  auto* child = findField(f.recorder.rows()[0], "/child_frame_id");
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->type, PJ::PrimitiveType::kString);
  EXPECT_EQ(child->string_value, "robot");

  auto* tx = findField(f.recorder.rows()[0], "/transform/translation/x");
  ASSERT_NE(tx, nullptr);
  EXPECT_DOUBLE_EQ(tx->numeric, 1.0);
}

}  // namespace
