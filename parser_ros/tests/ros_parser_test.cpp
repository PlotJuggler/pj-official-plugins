#include <gtest/gtest.h>

#include <any>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
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

TEST(RosParserTest, CameraInfoProducesObject) {
  static const char* kCameraInfoDef =
      "std_msgs/Header header\nuint32 height\nuint32 width\nstring distortion_model\n"
      "float64[] d\nfloat64[9] k\nfloat64[9] r\nfloat64[12] p\n"
      "uint32 binning_x\nuint32 binning_y\nsensor_msgs/RegionOfInterest roi\n"
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: sensor_msgs/RegionOfInterest\n"
      "uint32 x_offset\nuint32 y_offset\nuint32 height\nuint32 width\nbool do_rectify\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kCameraInfoDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("sensor_msgs/CameraInfo", kCameraInfoDef));
  EXPECT_EQ(f.handle.classifySchema("sensor_msgs/CameraInfo", def_span), PJ::sdk::BuiltinObjectType::kCameraInfo);

  const std::vector<double> kD = {0.1, 0.2, 0.3, 0.4, 0.5};
  const std::vector<double> kK = {500.0, 0.0, 320.0, 0.0, 500.0, 240.0, 0.0, 0.0, 1.0};
  const std::vector<double> kR = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  const std::vector<double> kP = {500.0, 0, 320.0, 0, 0, 500.0, 240.0, 0, 0, 0, 1.0, 0};

  // parseCameraInfo reads positionally and stops after P, so the payload need
  // only carry header..P (binning/roi are skipped by the single-message decode).
  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 5, 0, "camera_optical");
    enc.serializeUInt32(480);  // height
    enc.serializeUInt32(640);  // width
    enc.serializeString("plumb_bob");
    enc.serializeUInt32(static_cast<uint32_t>(kD.size()));  // D is a float64[] sequence
    for (double d : kD) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(d));
    }
    for (double k : kK) {  // K/R/P are fixed arrays — no count
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(k));
    }
    for (double r : kR) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(r));
    }
    for (double p : kP) {
      enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(p));
    }
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* ci = std::any_cast<PJ::sdk::CameraInfo>(&rec->object);
  ASSERT_NE(ci, nullptr);
  EXPECT_EQ(ci->frame_id, "camera_optical");
  EXPECT_EQ(ci->width, 640u);
  EXPECT_EQ(ci->height, 480u);
  EXPECT_EQ(ci->distortion_model, "plumb_bob");
  ASSERT_EQ(ci->D.size(), 5u);
  EXPECT_DOUBLE_EQ(ci->D[0], 0.1);
  EXPECT_DOUBLE_EQ(ci->D[4], 0.5);
  EXPECT_DOUBLE_EQ(ci->K[0], 500.0);
  EXPECT_DOUBLE_EQ(ci->K[2], 320.0);  // cx
  EXPECT_DOUBLE_EQ(ci->R[0], 1.0);
  EXPECT_DOUBLE_EQ(ci->P[0], 500.0);
  EXPECT_DOUBLE_EQ(ci->P[5], 500.0);  // fy
}

// ===== yolo_msgs/DetectionArray -> sdk::ImageAnnotations =====

// Full nested .msg definition for yolo_msgs/DetectionArray (mgonzs13/yolo_ros).
// Field order here must match what parseYoloDetectionArray reads positionally.
static const char* kYoloDetectionArrayDef =
    "std_msgs/Header header\nyolo_msgs/Detection[] detections\n"
    "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
    "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
    "================\nMSG: yolo_msgs/Detection\n"
    "int32 class_id\nstring class_name\nfloat64 score\nstring id\n"
    "yolo_msgs/BoundingBox2D bbox\nyolo_msgs/BoundingBox3D bbox3d\nyolo_msgs/Mask mask\n"
    "yolo_msgs/KeyPoint2DArray keypoints\nyolo_msgs/KeyPoint3DArray keypoints3d\n"
    "================\nMSG: yolo_msgs/BoundingBox2D\nyolo_msgs/Pose2D center\nyolo_msgs/Vector2 size\n"
    "================\nMSG: yolo_msgs/Pose2D\nyolo_msgs/Point2D position\nfloat64 theta\n"
    "================\nMSG: yolo_msgs/Point2D\nfloat64 x\nfloat64 y\n"
    "================\nMSG: yolo_msgs/Vector2\nfloat64 x\nfloat64 y\n"
    "================\nMSG: yolo_msgs/BoundingBox3D\ngeometry_msgs/Pose center\ngeometry_msgs/Vector3 size\nstring "
    "frame_id\n"
    "================\nMSG: geometry_msgs/Pose\ngeometry_msgs/Point position\ngeometry_msgs/Quaternion orientation\n"
    "================\nMSG: geometry_msgs/Point\nfloat64 x\nfloat64 y\nfloat64 z\n"
    "================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n"
    "================\nMSG: geometry_msgs/Vector3\nfloat64 x\nfloat64 y\nfloat64 z\n"
    "================\nMSG: yolo_msgs/Mask\nint32 height\nint32 width\nyolo_msgs/Point2D[] data\n"
    "================\nMSG: yolo_msgs/KeyPoint2DArray\nyolo_msgs/KeyPoint2D[] data\n"
    "================\nMSG: yolo_msgs/KeyPoint2D\nint32 id\nyolo_msgs/Point2D point\nfloat64 score\n"
    "================\nMSG: yolo_msgs/KeyPoint3DArray\nyolo_msgs/KeyPoint3D[] data\nstring frame_id\n"
    "================\nMSG: yolo_msgs/KeyPoint3D\nint32 id\ngeometry_msgs/Point point\nfloat64 score\n";

namespace {

void serializeF64(RosMsgParser::NanoCDR_Serializer& enc, double v) {
  enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(v));
}
void serializeI32(RosMsgParser::NanoCDR_Serializer& enc, int32_t v) {
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(v));
}

// Serializes one yolo_msgs/Detection exactly as parseYoloDetectionArray reads it.
struct YoloDet {
  int32_t class_id = 0;
  std::string class_name;
  double score = 0.0;
  double cx = 0.0, cy = 0.0, sx = 0.0, sy = 0.0;
  std::vector<std::pair<double, double>> mask;
  std::vector<std::pair<double, double>> keypoints2d;
};

void serializeYoloDetection(RosMsgParser::NanoCDR_Serializer& enc, const YoloDet& d) {
  serializeI32(enc, d.class_id);
  enc.serializeString(d.class_name);
  serializeF64(enc, d.score);
  enc.serializeString("");  // tracking id
  // bbox2d = Pose2D{Point2D{x,y}, theta} + Vector2{x,y}
  serializeF64(enc, d.cx);
  serializeF64(enc, d.cy);
  serializeF64(enc, 0.0);  // theta
  serializeF64(enc, d.sx);
  serializeF64(enc, d.sy);
  // bbox3d = Pose{Point(3)+Quaternion(4)} + Vector3(3) + frame_id
  for (int k = 0; k < 10; ++k) {
    serializeF64(enc, 0.0);
  }
  enc.serializeString("");  // bbox3d.frame_id
  // mask = int32 height + int32 width + Point2D[] data
  serializeI32(enc, 480);
  serializeI32(enc, 640);
  enc.serializeUInt32(static_cast<uint32_t>(d.mask.size()));
  for (const auto& p : d.mask) {
    serializeF64(enc, p.first);
    serializeF64(enc, p.second);
  }
  // keypoints2d = KeyPoint2D[] {int32 id, Point2D{x,y}, float64 score}
  enc.serializeUInt32(static_cast<uint32_t>(d.keypoints2d.size()));
  for (const auto& p : d.keypoints2d) {
    serializeI32(enc, 0);  // id
    serializeF64(enc, p.first);
    serializeF64(enc, p.second);
    serializeF64(enc, 0.5);  // score
  }
  // keypoints3d = KeyPoint3D[] (empty) + frame_id
  enc.serializeUInt32(0);
  enc.serializeString("");  // keypoints3d.frame_id
}

}  // namespace

TEST(RosParserTest, YoloDetectionArrayProducesImageAnnotations) {
  RosParserFixture f;
  f.setUp();
  const std::string def(kYoloDetectionArrayDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("yolo_msgs/DetectionArray", kYoloDetectionArrayDef));
  EXPECT_EQ(
      f.handle.classifySchema("yolo_msgs/DetectionArray", def_span), PJ::sdk::BuiltinObjectType::kImageAnnotations);

  YoloDet det;
  det.class_id = 1;
  det.class_name = "person";
  det.score = 0.9;
  det.cx = 100.0;
  det.cy = 50.0;
  det.sx = 40.0;
  det.sy = 20.0;
  det.mask = {{10.0, 10.0}, {20.0, 10.0}, {15.0, 25.0}};
  det.keypoints2d = {{12.0, 13.0}, {14.0, 15.0}};

  auto payload = serializeCdr([&det](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 7, 0, "camera_optical");
    enc.serializeUInt32(1);  // one detection
    serializeYoloDetection(enc, det);
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* ann = std::any_cast<PJ::sdk::ImageAnnotations>(&rec->object);
  ASSERT_NE(ann, nullptr);

  // The handler uses the Header.frame_id as the best-available image-topic hint.
  EXPECT_EQ(ann->image_topic, "camera_optical");

  // Box (LineLoop of 4 corners) + mask outline (LineLoop) = 2 PointsAnnotations.
  ASSERT_EQ(ann->points.size(), 2u);
  const auto& box = ann->points[0];
  EXPECT_EQ(box.topology, PJ::sdk::AnnotationTopology::kLineLoop);
  ASSERT_EQ(box.points.size(), 4u);
  EXPECT_DOUBLE_EQ(box.points[0].x, 80.0);   // cx - sx/2
  EXPECT_DOUBLE_EQ(box.points[0].y, 40.0);   // cy - sy/2
  EXPECT_DOUBLE_EQ(box.points[2].x, 120.0);  // cx + sx/2
  EXPECT_DOUBLE_EQ(box.points[2].y, 60.0);   // cy + sy/2

  const auto& mask = ann->points[1];
  EXPECT_EQ(mask.topology, PJ::sdk::AnnotationTopology::kLineLoop);
  ASSERT_EQ(mask.points.size(), 3u);
  EXPECT_DOUBLE_EQ(mask.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(mask.points[2].y, 25.0);

  // Label.
  ASSERT_EQ(ann->texts.size(), 1u);
  EXPECT_EQ(ann->texts[0].text, "person 0.90");
  EXPECT_DOUBLE_EQ(ann->texts[0].position.x, 80.0);

  // Two keypoints -> two filled circles.
  ASSERT_EQ(ann->circles.size(), 2u);
  EXPECT_DOUBLE_EQ(ann->circles[0].center.x, 12.0);
  EXPECT_DOUBLE_EQ(ann->circles[1].center.y, 15.0);
}

TEST(RosParserTest, YoloDetectionArrayBoxOnlyNoMaskNoKeypoints) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("yolo_msgs/DetectionArray", kYoloDetectionArrayDef));

  YoloDet a;
  a.class_id = 0;
  a.class_name = "cat";
  a.score = 0.5;
  a.cx = 200.0;
  a.cy = 200.0;
  a.sx = 10.0;
  a.sy = 10.0;
  YoloDet b = a;
  b.class_name = "dog";

  auto payload = serializeCdr([&a, &b](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 1, 0, "cam");
    enc.serializeUInt32(2);  // two detections
    serializeYoloDetection(enc, a);
    serializeYoloDetection(enc, b);
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(99, view);
  ASSERT_TRUE(rec.has_value());

  const auto* ann = std::any_cast<PJ::sdk::ImageAnnotations>(&rec->object);
  ASSERT_NE(ann, nullptr);
  // Two detections, box only (no mask, no keypoints).
  EXPECT_EQ(ann->points.size(), 2u);
  EXPECT_EQ(ann->texts.size(), 2u);
  EXPECT_TRUE(ann->circles.empty());
  EXPECT_EQ(ann->texts[0].text, "cat 0.50");
  EXPECT_EQ(ann->texts[1].text, "dog 0.50");
}

// Scalar companion for the object-only YOLO entry. Without a parse_scalars, the
// host's eager-scalar ingest (parse() -> parseScalars) fails and aborts the push
// on any non-kPureLazy policy (e.g. live streams), silently dropping the overlay.
// The slim companion emits num_detections so the object ingests under any policy.
TEST(RosParserTest, YoloDetectionArrayScalarRouteEmitsNumDetections) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("yolo_msgs/DetectionArray", kYoloDetectionArrayDef));

  YoloDet d;
  d.class_id = 1;
  d.class_name = "person";
  d.score = 0.9;
  d.cx = 100.0;
  d.cy = 50.0;
  d.sx = 40.0;
  d.sy = 20.0;
  auto payload = serializeCdr([&d](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 7, 0, "camera_optical");
    enc.serializeUInt32(2);  // two detections
    serializeYoloDetection(enc, d);
    serializeYoloDetection(enc, d);
  });

  // parse() drives the scalar route; it would FAIL (push abort) without the
  // companion parse_scalars on this object-only schema.
  ASSERT_TRUE(f.parse(payload, 1234));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto* num = PJ::sdk::testing::ParserWriteRecorder::findField(f.recorder.rows()[0], "num_detections");
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->numeric, 2.0);
}

// frame_id producer: parseImage must carry Header.frame_id into the canonical
// sdk::Image so a consumer can match the image to its CameraInfo / place it in 3D.
TEST(RosParserTest, ImageObjectCarriesFrameId) {
  static const char* kImageDef =
      "std_msgs/Header header\nuint32 height\nuint32 width\nstring encoding\n"
      "uint8 is_bigendian\nuint32 step\nuint8[] data\n"
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n";

  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/Image", kImageDef));

  const std::vector<uint8_t> pixels = {0x10, 0x20, 0x30, 0x40};  // 2x2 mono8 = step(2) * height(2)
  auto payload = serializeCdr([&pixels](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 7, 0, "camera_link");
    enc.serializeUInt32(2);  // height
    enc.serializeUInt32(2);  // width
    enc.serializeString("mono8");
    enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(0)));  // is_bigendian
    enc.serializeUInt32(2);                                                              // step
    enc.serializeUInt32(static_cast<uint32_t>(pixels.size()));                           // uint8[] data: count
    for (uint8_t b : pixels) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(b));
    }
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* img = std::any_cast<PJ::sdk::Image>(&rec->object);
  ASSERT_NE(img, nullptr);
  EXPECT_EQ(img->frame_id, "camera_link");
  EXPECT_EQ(img->width, 2u);
  EXPECT_EQ(img->height, 2u);
  EXPECT_EQ(img->encoding, "mono8");
}

TEST(RosParserTest, CompressedVideoProducesObject) {
  // foxglove_msgs/CompressedVideo. The first field is a BARE
  // builtin_interfaces/Time (sec, nanosec) — NOT a std_msgs/Header — followed
  // by frame_id, the compressed uint8[] data, and the format string.
  static const char* kCompressedVideoDef =
      "builtin_interfaces/Time timestamp\nstring frame_id\nuint8[] data\nstring format\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kCompressedVideoDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("foxglove_msgs/CompressedVideo", kCompressedVideoDef));
  EXPECT_EQ(
      f.handle.classifySchema("foxglove_msgs/CompressedVideo", def_span), PJ::sdk::BuiltinObjectType::kVideoFrame);

  // A small, recognizable H.264-ish blob. Only its verbatim round-trip and
  // zero-copy aliasing matter here.
  const std::vector<uint8_t> bitstream = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xDE, 0xAD, 0xBE, 0xEF};
  auto payload = serializeCdr([&bitstream](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(7)));     // timestamp.sec
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(42)));  // timestamp.nanosec
    enc.serializeString("camera_optical");                                                  // frame_id
    enc.serializeUInt32(static_cast<uint32_t>(bitstream.size()));                           // uint8[] data: count
    for (uint8_t b : bitstream) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(b));
    }
    enc.serializeString("h264");  // format
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* vf = std::any_cast<PJ::sdk::VideoFrame>(&rec->object);
  ASSERT_NE(vf, nullptr);
  EXPECT_EQ(vf->frame_id, "camera_optical");
  EXPECT_EQ(vf->format, "h264");
  ASSERT_EQ(vf->data.size(), bitstream.size());
  for (size_t i = 0; i < bitstream.size(); ++i) {
    EXPECT_EQ(vf->data.data()[i], bitstream[i]);
  }
  // Zero-copy: the decoded data span must alias the CDR payload buffer, not a
  // fresh copy. The bytes live inside `payload` at the uint8[] body offset.
  EXPECT_GE(vf->data.data(), payload.data());
  EXPECT_LE(vf->data.data() + vf->data.size(), payload.data() + payload.size());
}

TEST(RosParserTest, CompressedVideoEmbeddedTimestamp) {
  static const char* kCompressedVideoDef =
      "builtin_interfaces/Time timestamp\nstring frame_id\nuint8[] data\nstring format\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n";

  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.bindSchema("foxglove_msgs/CompressedVideo", kCompressedVideoDef));

  const std::vector<uint8_t> bitstream = {0x01, 0x02, 0x03};
  auto payload = serializeCdr([&bitstream](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(5)));            // sec
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(250000000)));  // nanosec
    enc.serializeString("cam");
    enc.serializeUInt32(static_cast<uint32_t>(bitstream.size()));
    for (uint8_t b : bitstream) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(b));
    }
    enc.serializeString("av1");
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(9999, view);
  ASSERT_TRUE(rec.has_value());
  // 5s + 250ms embedded -> 5'250'000'000 ns, overriding the host ts (9999).
  ASSERT_TRUE(rec->ts.has_value());
  EXPECT_EQ(*rec->ts, 5'250'000'000LL);

  const auto* vf = std::any_cast<PJ::sdk::VideoFrame>(&rec->object);
  ASSERT_NE(vf, nullptr);
  EXPECT_EQ(vf->format, "av1");
  EXPECT_EQ(vf->timestamp_ns, 5'250'000'000LL);
}

TEST(RosParserTest, FoxgloveCompressedPointCloudProducesObject) {
  // foxglove_msgs/CompressedPointCloud. First field is a BARE
  // builtin_interfaces/Time, then frame_id, a geometry_msgs/Pose (read +
  // dropped), the compressed uint8[] blob, and finally the format string.
  static const char* kDef =
      "builtin_interfaces/Time timestamp\nstring frame_id\ngeometry_msgs/Pose pose\n"
      "uint8[] data\nstring format\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: geometry_msgs/Pose\n"
      "geometry_msgs/Point position\ngeometry_msgs/Quaternion orientation\n"
      "================\nMSG: geometry_msgs/Point\nfloat64 x\nfloat64 y\nfloat64 z\n"
      "================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("foxglove_msgs/CompressedPointCloud", kDef));
  EXPECT_EQ(
      f.handle.classifySchema("foxglove_msgs/CompressedPointCloud", def_span),
      PJ::sdk::BuiltinObjectType::kCompressedPointCloud);

  const std::vector<uint8_t> blob = {0x44, 0x52, 0x41, 0x43, 0xDE, 0xAD, 0xBE, 0xEF, 0x01};  // "DRAC" + junk
  auto payload = serializeCdr([&blob](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(static_cast<int32_t>(7)));     // timestamp.sec
    enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(static_cast<uint32_t>(42)));  // timestamp.nanosec
    enc.serializeString("lidar_frame");                                                     // frame_id
    serializeVector3(enc, 1.0, 2.0, 3.0);                                                   // pose.position (dropped)
    serializeQuaternion(enc, 0.0, 0.0, 0.0, 1.0);             // pose.orientation (dropped)
    enc.serializeUInt32(static_cast<uint32_t>(blob.size()));  // uint8[] data: count
    for (uint8_t b : blob) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(b));
    }
    enc.serializeString("draco");  // format
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* cloud = std::any_cast<PJ::sdk::CompressedPointCloud>(&rec->object);
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(cloud->frame_id, "lidar_frame");
  EXPECT_EQ(cloud->format, "draco");
  ASSERT_EQ(cloud->data.size(), blob.size());
  for (size_t i = 0; i < blob.size(); ++i) {
    EXPECT_EQ(cloud->data.data()[i], blob[i]);
  }
  // Zero-copy: the blob span must alias the CDR payload buffer.
  EXPECT_GE(cloud->data.data(), payload.data());
  EXPECT_LE(cloud->data.data() + cloud->data.size(), payload.data() + payload.size());
}

TEST(RosParserTest, CompressedPointCloud2ProducesObject) {
  // point_cloud_interfaces/CompressedPointCloud2. Header first, then layout
  // metadata + a PointField[] (all read and discarded), the compressed blob,
  // is_dense, and finally the format string.
  static const char* kDef =
      "std_msgs/Header header\nuint32 height\nuint32 width\nsensor_msgs/PointField[] fields\n"
      "bool is_bigendian\nuint32 point_step\nuint32 row_step\nuint8[] compressed_data\n"
      "bool is_dense\nstring format\n"
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: sensor_msgs/PointField\nstring name\nuint32 offset\nuint8 datatype\nuint32 count\n";

  RosParserFixture f;
  f.setUp();
  const std::string def(kDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("point_cloud_interfaces/CompressedPointCloud2", kDef));
  EXPECT_EQ(
      f.handle.classifySchema("point_cloud_interfaces/CompressedPointCloud2", def_span),
      PJ::sdk::BuiltinObjectType::kCompressedPointCloud);

  const std::vector<uint8_t> blob = {0xCA, 0xFE, 0xBA, 0xBE, 0x10, 0x20, 0x30};
  auto payload = serializeCdr([&blob](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 11, 500000000, "velodyne");  // header
    enc.serializeUInt32(1);                           // height
    enc.serializeUInt32(2048);                        // width
    // fields[]: two PointField entries (x, y) — read and discarded by the handler.
    enc.serializeUInt32(2);  // fields count
    for (const char* fname : {"x", "y"}) {
      enc.serializeString(fname);                                                          // name
      enc.serializeUInt32(0);                                                              // offset
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(7)));  // datatype FLOAT32
      enc.serializeUInt32(1);                                                              // count
    }
    enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(0)));  // is_bigendian
    enc.serializeUInt32(16);                                                             // point_step
    enc.serializeUInt32(32768);                                                          // row_step
    enc.serializeUInt32(static_cast<uint32_t>(blob.size()));                             // compressed_data: count
    for (uint8_t b : blob) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(b));
    }
    enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(1)));  // is_dense
    enc.serializeString("cloudini");                                                     // format
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value());

  const auto* cloud = std::any_cast<PJ::sdk::CompressedPointCloud>(&rec->object);
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(cloud->frame_id, "velodyne");
  EXPECT_EQ(cloud->format, "cloudini");
  ASSERT_EQ(cloud->data.size(), blob.size());
  for (size_t i = 0; i < blob.size(); ++i) {
    EXPECT_EQ(cloud->data.data()[i], blob[i]);
  }
  // Zero-copy: the blob span must alias the CDR payload buffer.
  EXPECT_GE(cloud->data.data(), payload.data());
  EXPECT_LE(cloud->data.data() + cloud->data.size(), payload.data() + payload.size());
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

TEST(RosParserTest, RobotDescriptionNamespacedTopicProducesObject) {
  RosParserFixture f;
  f.setUp();
  // A namespace-prefixed robot_description topic is matched too.
  ASSERT_TRUE(f.handle.loadConfig(R"({"topic_name":"/my_robot/robot_description"})"));

  const std::string def = "string data\n";
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("std_msgs/String", def));
  EXPECT_EQ(f.handle.classifySchema("std_msgs/String", def_span), PJ::sdk::BuiltinObjectType::kRobotDescription);

  const std::string sdf = "<sdf version=\"1.6\"><model name=\"m\"/></sdf>";
  auto payload = serializeCdr([&sdf](RosMsgParser::NanoCDR_Serializer& enc) { enc.serializeString(sdf); });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1, view);
  ASSERT_TRUE(rec.has_value());

  const auto* rd = std::any_cast<PJ::sdk::RobotDescription>(&rec->object);
  ASSERT_NE(rd, nullptr);
  EXPECT_EQ(rd->topic, "/my_robot/robot_description");
  EXPECT_EQ(rd->format, "sdf");  // also exercises the SDF format sniff
  EXPECT_EQ(rd->text, sdf);
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

// ===== visualization_msgs/Marker(Array) -> sdk::SceneEntities =====

// Builds a visualization_msgs/Marker .msg definition. `humble` adds the
// texture block (texture_resource/texture/uv_coordinates) and mesh_file field
// that ROS 2 humble+ carry but EOL foxy/galactic do not.
std::string makeMarkerFields(bool humble) {
  std::string s =
      "std_msgs/Header header\nstring ns\nint32 id\nint32 type\nint32 action\n"
      "geometry_msgs/Pose pose\ngeometry_msgs/Vector3 scale\nstd_msgs/ColorRGBA color\n"
      "builtin_interfaces/Duration lifetime\nbool frame_locked\n"
      "geometry_msgs/Point[] points\nstd_msgs/ColorRGBA[] colors\n";
  if (humble) {
    s += "string texture_resource\nsensor_msgs/CompressedImage texture\n"
         "visualization_msgs/UVCoordinate[] uv_coordinates\n";
  }
  s += "string text\nstring mesh_resource\n";
  if (humble) {
    s += "visualization_msgs/MeshFile mesh_file\n";
  }
  s += "bool mesh_use_embedded_materials\n";
  return s;
}

std::string makeMarkerNested(bool humble) {
  std::string s =
      "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
      "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: builtin_interfaces/Duration\nint32 sec\nuint32 nanosec\n"
      "================\nMSG: geometry_msgs/Pose\ngeometry_msgs/Point position\ngeometry_msgs/Quaternion orientation\n"
      "================\nMSG: geometry_msgs/Point\nfloat64 x\nfloat64 y\nfloat64 z\n"
      "================\nMSG: geometry_msgs/Quaternion\nfloat64 x\nfloat64 y\nfloat64 z\nfloat64 w\n"
      "================\nMSG: geometry_msgs/Vector3\nfloat64 x\nfloat64 y\nfloat64 z\n"
      "================\nMSG: std_msgs/ColorRGBA\nfloat32 r\nfloat32 g\nfloat32 b\nfloat32 a\n";
  if (humble) {
    s += "================\nMSG: sensor_msgs/CompressedImage\nstd_msgs/Header header\nstring format\nuint8[] data\n"
         "================\nMSG: visualization_msgs/UVCoordinate\nfloat32 u\nfloat32 v\n"
         "================\nMSG: visualization_msgs/MeshFile\nstring filename\nuint8[] data\n";
  }
  return s;
}

std::string markerDef(bool humble) {
  return makeMarkerFields(humble) + makeMarkerNested(humble);
}

std::string markerArrayDef(bool humble) {
  return std::string("visualization_msgs/Marker[] markers\n") + "================\nMSG: visualization_msgs/Marker\n" +
         makeMarkerFields(humble) + makeMarkerNested(humble);
}

struct MarkerWire {
  int32_t id = 0;
  int32_t type = 0;
  int32_t action = 0;
  std::string ns = "ns";
  std::string frame_id = "world";
  int32_t sec = 1;
  uint32_t nsec = 0;
  std::array<double, 7> pose{{0, 0, 0, 0, 0, 0, 1}};  // position xyz, orientation xyzw
  std::array<double, 3> scale{{1, 1, 1}};
  std::array<float, 4> color{{1, 1, 1, 1}};
  int32_t life_sec = 0;
  uint32_t life_nsec = 0;
  bool frame_locked = false;
  std::vector<std::array<double, 3>> points;
  std::vector<std::array<float, 4>> colors;
  std::string text;
  std::string mesh_resource;
  std::string mesh_filename;
  std::vector<uint8_t> mesh_file_data;
  bool mesh_use_embedded = false;
};

// Serializes one Marker in CDR exactly as decodeOneMarker reads it. The two
// layout flags must match the bound definition.
void serializeMarker(
    RosMsgParser::NanoCDR_Serializer& enc, const MarkerWire& m, bool has_texture_block, bool has_mesh_file) {
  serializeHeader(enc, m.sec, m.nsec, m.frame_id);
  enc.serializeString(m.ns);
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(m.id));
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(m.type));
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(m.action));
  for (double v : m.pose) {
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(v));
  }
  for (double v : m.scale) {
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(v));
  }
  for (float v : m.color) {
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(v));
  }
  enc.serialize(RosMsgParser::INT32, RosMsgParser::Variant(m.life_sec));
  enc.serialize(RosMsgParser::UINT32, RosMsgParser::Variant(m.life_nsec));
  enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(m.frame_locked ? 1 : 0)));

  enc.serializeUInt32(static_cast<uint32_t>(m.points.size()));
  for (const auto& p : m.points) {
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(p[0]));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(p[1]));
    enc.serialize(RosMsgParser::FLOAT64, RosMsgParser::Variant(p[2]));
  }
  enc.serializeUInt32(static_cast<uint32_t>(m.colors.size()));
  for (const auto& c : m.colors) {
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(c[0]));
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(c[1]));
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(c[2]));
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(c[3]));
  }

  if (has_texture_block) {
    enc.serializeString("");         // texture_resource
    serializeHeader(enc, 0, 0, "");  // texture.header
    enc.serializeString("");         // texture.format
    enc.serializeUInt32(0);          // texture.data (empty byte sequence)
    enc.serializeUInt32(0);          // uv_coordinates (empty)
  }
  enc.serializeString(m.text);
  enc.serializeString(m.mesh_resource);
  if (has_mesh_file) {
    enc.serializeString(m.mesh_filename);                                 // mesh_file.filename
    enc.serializeUInt32(static_cast<uint32_t>(m.mesh_file_data.size()));  // mesh_file.data length
    for (uint8_t byte : m.mesh_file_data) {
      enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(byte));
    }
  }
  enc.serialize(RosMsgParser::UINT8, RosMsgParser::Variant(static_cast<uint8_t>(m.mesh_use_embedded ? 1 : 0)));
}

const PJ::sdk::SceneEntities* parseSceneEntities(
    RosParserFixture& f, const std::vector<uint8_t>& payload, std::any& hold) {
  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  if (base == nullptr) {
    return nullptr;
  }
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1000, view);
  if (!rec.has_value()) {
    return nullptr;
  }
  hold = std::move(rec->object);
  return std::any_cast<PJ::sdk::SceneEntities>(&hold);
}

TEST(RosParserTest, MarkerArrayProducesSceneEntities) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerArrayDef(/*humble=*/true);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("visualization_msgs/MarkerArray", def));
  EXPECT_EQ(
      f.handle.classifySchema("visualization_msgs/MarkerArray", def_span), PJ::sdk::BuiltinObjectType::kSceneEntities);

  MarkerWire cube;
  cube.ns = "a";
  cube.id = 1;
  cube.type = 1;  // CUBE
  cube.sec = 1;
  cube.nsec = 500;
  cube.scale = {2.0, 3.0, 4.0};
  cube.color = {1.0f, 0.0f, 0.0f, 1.0f};

  MarkerWire sphere;
  sphere.ns = "a";
  sphere.id = 2;
  sphere.type = 2;  // SPHERE
  sphere.scale = {0.5, 0.5, 0.5};

  MarkerWire line;
  line.ns = "a";
  line.id = 3;
  line.type = 4;  // LINE_STRIP
  line.scale = {0.05, 0.0, 0.0};
  line.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeUInt32(3);
    serializeMarker(enc, cube, true, true);
    serializeMarker(enc, sphere, true, true);
    serializeMarker(enc, line, true, true);
  });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 3u);
  EXPECT_TRUE(se->deletions.empty());

  EXPECT_EQ(se->entities[0].id, "1:a:1");
  EXPECT_EQ(se->entities[0].frame_id, "world");
  EXPECT_EQ(se->entities[0].timestamp, 1'000'000'500);
  ASSERT_EQ(se->entities[0].cubes.size(), 1u);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[0].size.x, 2.0);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[0].size.z, 4.0);
  EXPECT_EQ(se->entities[0].cubes[0].color.r, 255);
  EXPECT_EQ(se->entities[0].cubes[0].color.g, 0);

  ASSERT_EQ(se->entities[1].spheres.size(), 1u);
  EXPECT_DOUBLE_EQ(se->entities[1].spheres[0].size.x, 0.5);

  ASSERT_EQ(se->entities[2].lines.size(), 1u);
  EXPECT_EQ(se->entities[2].lines[0].type, PJ::sdk::LineType::kLineStrip);
  ASSERT_EQ(se->entities[2].lines[0].points.size(), 3u);
  EXPECT_DOUBLE_EQ(se->entities[2].lines[0].points[2].y, 1.0);
  EXPECT_DOUBLE_EQ(se->entities[2].lines[0].thickness, 0.05);
}

TEST(RosParserTest, TextMarkerHumbleAndFoxyLayouts) {
  for (bool humble : {true, false}) {
    RosParserFixture f;
    f.setUp();
    const std::string def = markerDef(humble);
    ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def)) << (humble ? "humble" : "foxy");

    MarkerWire text;
    text.type = 9;  // TEXT_VIEW_FACING
    text.scale = {0.0, 0.0, 0.25};
    text.text = "hello";

    auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
      serializeMarker(enc, text, /*has_texture_block=*/humble, /*has_mesh_file=*/humble);
    });

    std::any hold;
    const auto* se = parseSceneEntities(f, payload, hold);
    ASSERT_NE(se, nullptr) << (humble ? "humble" : "foxy");
    ASSERT_EQ(se->entities.size(), 1u);
    ASSERT_EQ(se->entities[0].texts.size(), 1u);
    EXPECT_EQ(se->entities[0].texts[0].text, "hello");
    EXPECT_TRUE(se->entities[0].texts[0].billboard);
    EXPECT_DOUBLE_EQ(se->entities[0].texts[0].font_size, 0.25);
  }
}

TEST(RosParserTest, MeshResourceMarkerProducesModel) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def));

  MarkerWire mesh;
  mesh.type = 10;  // MESH_RESOURCE
  mesh.scale = {1.0, 1.0, 1.0};
  mesh.mesh_resource = "package://robot/meshes/base.dae";
  mesh.mesh_use_embedded = false;  // -> override_color = true

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) { serializeMarker(enc, mesh, true, true); });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 1u);
  ASSERT_EQ(se->entities[0].models.size(), 1u);
  EXPECT_EQ(se->entities[0].models[0].url, "package://robot/meshes/base.dae");
  EXPECT_TRUE(se->entities[0].models[0].override_color);
}

// ROS 2 humble+ Markers can embed the mesh bytes inline via mesh_file (no
// resolvable URL needed). Pre-fix decodeOneMarker read and DISCARDED mesh_file,
// so the model had empty data and nothing rendered. The bytes must survive
// whole and the media_type be inferred from the filename extension.
TEST(RosParserTest, MeshFileMarkerProducesEmbeddedModel) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def));

  const std::vector<uint8_t> glb = {'g', 'l', 'T', 'F', 0x02, 0x00, 0xAB, 0xCD, 0xEF};
  MarkerWire mesh;
  mesh.type = 10;  // MESH_RESOURCE
  mesh.scale = {1.0, 1.0, 1.0};
  mesh.mesh_filename = "car.glb";
  mesh.mesh_file_data = glb;      // embedded, no mesh_resource URL
  mesh.mesh_use_embedded = true;  // -> override_color = false

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) { serializeMarker(enc, mesh, true, true); });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 1u);
  ASSERT_EQ(se->entities[0].models.size(), 1u);  // embedded-only must still produce a model (guard fix)
  const auto& model = se->entities[0].models[0];
  EXPECT_EQ(model.data, glb);  // full bytes, not truncated/dropped
  EXPECT_EQ(model.media_type, "model/gltf-binary");
  EXPECT_TRUE(model.url.empty());
  EXPECT_FALSE(model.override_color);
}

// When a publisher provides BOTH mesh_resource (url) and mesh_file (bytes), the
// faithful translation keeps both — the consumer prefers data when present.
TEST(RosParserTest, MeshFileAndUrlMarkerKeepsBoth) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def));

  const std::vector<uint8_t> stl = {0x73, 0x6F, 0x6C, 0x69, 0x64};  // "solid"
  MarkerWire mesh;
  mesh.type = 10;
  mesh.scale = {1.0, 1.0, 1.0};
  mesh.mesh_resource = "package://robot/meshes/arm.stl";
  mesh.mesh_filename = "arm.stl";
  mesh.mesh_file_data = stl;

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) { serializeMarker(enc, mesh, true, true); });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 1u);
  ASSERT_EQ(se->entities[0].models.size(), 1u);
  const auto& model = se->entities[0].models[0];
  EXPECT_EQ(model.data, stl);
  EXPECT_EQ(model.media_type, "model/stl");
  EXPECT_EQ(model.url, "package://robot/meshes/arm.stl");
}

// A non-mesh marker may still carry non-empty mesh_file bytes (nonconforming
// publisher). The parser must consume them for wire alignment without copying
// or attaching them to anything, and the NEXT marker in the array must decode
// intact. Guards the gating of the mesh_file copy to MESH_RESOURCE markers.
TEST(RosParserTest, NonMeshMarkerWithMeshFileBytesStaysAligned) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerArrayDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/MarkerArray", def));

  MarkerWire cube;
  cube.ns = "a";
  cube.id = 1;
  cube.type = 1;  // CUBE — not MESH_RESOURCE...
  cube.scale = {2.0, 3.0, 4.0};
  cube.mesh_filename = "ignored.glb";
  cube.mesh_file_data = {0xDE, 0xAD, 0xBE, 0xEF};  // ...yet mesh_file is non-empty

  MarkerWire sphere;
  sphere.ns = "a";
  sphere.id = 2;
  sphere.type = 2;  // SPHERE
  sphere.scale = {0.5, 0.5, 0.5};

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeUInt32(2);
    serializeMarker(enc, cube, true, true);
    serializeMarker(enc, sphere, true, true);
  });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 2u);
  ASSERT_EQ(se->entities[0].cubes.size(), 1u);
  EXPECT_TRUE(se->entities[0].models.empty());    // stray bytes not attached to the cube
  ASSERT_EQ(se->entities[1].spheres.size(), 1u);  // next marker stayed aligned
  EXPECT_DOUBLE_EQ(se->entities[1].spheres[0].size.x, 0.5);
}

TEST(RosParserTest, MarkerDeleteAndDeleteAll) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerArrayDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/MarkerArray", def));

  MarkerWire del;
  del.ns = "a";
  del.id = 5;
  del.action = 2;  // DELETE
  del.sec = 7;

  MarkerWire del_all;
  del_all.action = 3;  // DELETEALL

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    enc.serializeUInt32(2);
    serializeMarker(enc, del, true, true);
    serializeMarker(enc, del_all, true, true);
  });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  EXPECT_TRUE(se->entities.empty());
  ASSERT_EQ(se->deletions.size(), 2u);
  EXPECT_EQ(se->deletions[0].type, PJ::sdk::SceneEntityDeletion::Type::kMatchingId);
  EXPECT_EQ(se->deletions[0].id, "1:a:5");
  EXPECT_EQ(se->deletions[0].timestamp, 7'000'000'000);
  EXPECT_EQ(se->deletions[1].type, PJ::sdk::SceneEntityDeletion::Type::kAll);
}

TEST(RosParserTest, CubeListPerPointColors) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def));

  MarkerWire list;
  list.type = 6;  // CUBE_LIST
  list.scale = {0.1, 0.1, 0.1};
  list.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
  list.colors = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) { serializeMarker(enc, list, true, true); });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  ASSERT_EQ(se->entities.size(), 1u);
  ASSERT_EQ(se->entities[0].cubes.size(), 2u);
  EXPECT_EQ(se->entities[0].cubes[0].color.r, 255);
  EXPECT_EQ(se->entities[0].cubes[1].color.g, 255);
  EXPECT_DOUBLE_EQ(se->entities[0].cubes[1].pose.position.x, 1.0);
}

TEST(RosParserTest, PointsMarkerSkipped) {
  RosParserFixture f;
  f.setUp();
  const std::string def = markerDef(/*humble=*/true);
  ASSERT_TRUE(f.bindSchema("visualization_msgs/Marker", def));

  MarkerWire pts;
  pts.type = 8;  // POINTS
  pts.points = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};

  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) { serializeMarker(enc, pts, true, true); });

  std::any hold;
  const auto* se = parseSceneEntities(f, payload, hold);
  ASSERT_NE(se, nullptr);
  EXPECT_TRUE(se->entities.empty());
  EXPECT_TRUE(se->deletions.empty());
}

// ===== sensor_msgs/LaserScan -> sdk::PointCloud (eager projection) =====

static const char* kLaserScanDef =
    "std_msgs/Header header\n"
    "float32 angle_min\nfloat32 angle_max\nfloat32 angle_increment\n"
    "float32 time_increment\nfloat32 scan_time\n"
    "float32 range_min\nfloat32 range_max\n"
    "float32[] ranges\nfloat32[] intensities\n"
    "================\nMSG: std_msgs/Header\nbuiltin_interfaces/Time stamp\nstring frame_id\n"
    "================\nMSG: builtin_interfaces/Time\nint32 sec\nuint32 nanosec\n";

// Serializes the LaserScan body after the header: the 7 float32 params plus
// the two variable-length float32 arrays.
void serializeLaserScanBody(
    RosMsgParser::NanoCDR_Serializer& enc, float angle_min, float angle_increment, float range_min, float range_max,
    const std::vector<float>& ranges, const std::vector<float>& intensities) {
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(angle_min));
  const float angle_max = angle_min + angle_increment * static_cast<float>(ranges.empty() ? 0 : ranges.size() - 1);
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(angle_max));
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(angle_increment));
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(0.0f));  // time_increment
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(0.1f));  // scan_time
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(range_min));
  enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(range_max));
  enc.serializeUInt32(static_cast<uint32_t>(ranges.size()));
  for (float r : ranges) {
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(r));
  }
  enc.serializeUInt32(static_cast<uint32_t>(intensities.size()));
  for (float v : intensities) {
    enc.serialize(RosMsgParser::FLOAT32, RosMsgParser::Variant(v));
  }
}

// Reads the float32 at byte offset `off` of point `index` in a packed cloud.
float laserCloudFloat(const PJ::sdk::PointCloud& cloud, uint32_t index, uint32_t off) {
  float v = 0.0f;
  std::memcpy(&v, cloud.data.data() + static_cast<size_t>(index) * cloud.point_step + off, sizeof(float));
  return v;
}

TEST(RosParserTest, LaserScanProducesPointCloudObject) {
  RosParserFixture f;
  f.setUp();
  const std::string def(kLaserScanDef);
  const PJ::Span<const uint8_t> def_span(reinterpret_cast<const uint8_t*>(def.data()), def.size());
  ASSERT_TRUE(f.bindSchema("sensor_msgs/LaserScan", kLaserScanDef));
  EXPECT_EQ(f.handle.classifySchema("sensor_msgs/LaserScan", def_span), PJ::sdk::BuiltinObjectType::kPointCloud);

  // 5 rays; NaN (i=1), below-min (i=2) and above-max (i=3) must drop.
  const std::vector<float> ranges = {1.0f, std::numeric_limits<float>::quiet_NaN(), 0.2f, 11.0f, 2.0f};
  auto payload = serializeCdr([&ranges](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 11, 22, "laser");
    serializeLaserScanBody(enc, /*angle_min=*/-1.0f, /*angle_increment=*/0.5f, 0.5f, 10.0f, ranges, {});
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_FALSE(rec->ts.has_value());  // embedded ts disabled by default

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->frame_id, "laser");
  EXPECT_EQ(pc->timestamp_ns, 1234);
  EXPECT_EQ(pc->height, 1u);
  EXPECT_EQ(pc->width, 2u);  // rays 0 and 4 kept
  EXPECT_EQ(pc->point_step, 12u);
  EXPECT_EQ(pc->row_step, 2u * 12u);
  EXPECT_TRUE(pc->is_dense);
  ASSERT_EQ(pc->fields.size(), 3u);
  EXPECT_EQ(pc->fields[0].name, "x");
  EXPECT_EQ(pc->fields[1].name, "y");
  EXPECT_EQ(pc->fields[2].name, "z");

  // Ray 0: theta = -1.0; ray 4: theta = -1.0 + 4*0.5 = 1.0 (double math, float output).
  EXPECT_EQ(laserCloudFloat(*pc, 0, 0), 1.0f * static_cast<float>(std::cos(-1.0)));
  EXPECT_EQ(laserCloudFloat(*pc, 0, 4), 1.0f * static_cast<float>(std::sin(-1.0)));
  EXPECT_EQ(laserCloudFloat(*pc, 0, 8), 0.0f);
  EXPECT_EQ(laserCloudFloat(*pc, 1, 0), 2.0f * static_cast<float>(std::cos(1.0)));
  EXPECT_EQ(laserCloudFloat(*pc, 1, 4), 2.0f * static_cast<float>(std::sin(1.0)));
}

TEST(RosParserTest, LaserScanIntensitiesPassThrough) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/LaserScan", kLaserScanDef));

  const std::vector<float> ranges = {1.0f, std::numeric_limits<float>::infinity(), 3.0f};
  const std::vector<float> intensities = {10.0f, 20.0f, 30.0f};
  auto payload = serializeCdr([&](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 1, 0, "laser");
    serializeLaserScanBody(enc, 0.0f, 0.1f, 0.5f, 10.0f, ranges, intensities);
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->width, 2u);  // Inf ray dropped
  EXPECT_EQ(pc->point_step, 16u);
  ASSERT_EQ(pc->fields.size(), 4u);
  EXPECT_EQ(pc->fields[3].name, "intensity");
  EXPECT_EQ(pc->fields[3].offset, 12u);
  // Intensity follows its ray through the drop filter.
  EXPECT_EQ(laserCloudFloat(*pc, 0, 12), 10.0f);
  EXPECT_EQ(laserCloudFloat(*pc, 1, 12), 30.0f);
}

TEST(RosParserTest, LaserScanEmptyScanProducesEmptyCloud) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/LaserScan", kLaserScanDef));

  auto payload = serializeCdr([](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 1, 0, "laser");
    serializeLaserScanBody(enc, 0.0f, 0.1f, 0.5f, 10.0f, {}, {});
  });

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->width, 0u);
  EXPECT_EQ(pc->data.size(), 0u);
  EXPECT_TRUE(pc->is_dense);
}

// The catalog entry must keep the scalar route on the generic flatten (the
// pre-existing behavior for LaserScan): angle params and ranges[i] columns
// still appear, subject to the user-configured array policy.
TEST(RosParserTest, LaserScanScalarRouteKeepsGenericFlatten) {
  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/LaserScan", kLaserScanDef));

  const std::vector<float> ranges = {1.5f, 2.5f};
  auto payload = serializeCdr([&ranges](RosMsgParser::NanoCDR_Serializer& enc) {
    serializeHeader(enc, 1, 0, "laser");
    serializeLaserScanBody(enc, -0.5f, 0.25f, 0.1f, 20.0f, ranges, {});
  });

  ASSERT_TRUE(f.parse(payload, 99));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];

  const auto* angle_min = PJ::sdk::testing::ParserWriteRecorder::findField(row, "/angle_min");
  ASSERT_NE(angle_min, nullptr);
  EXPECT_FLOAT_EQ(static_cast<float>(angle_min->numeric), -0.5f);

  const auto* range0 = PJ::sdk::testing::ParserWriteRecorder::findField(row, "/ranges[0]");
  ASSERT_NE(range0, nullptr);
  EXPECT_FLOAT_EQ(static_cast<float>(range0->numeric), 1.5f);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "/header/frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "laser");
}

// Golden fixture: one real CDR sensor_msgs/msg/LaserScan message captured from
// a ROS 2 recording (1440 rays, range [0.3, 40.0], empty intensities,
// frame_id "base_link", header stamp 1779975681.103199244). Exercises the real
// wire alignment end to end.
TEST(RosParserTest, LaserScanRealCdrMessageFromBag) {
  std::ifstream in(std::string(PJ_ROS_PARSER_TEST_DATA_DIR) + "/laserscan_real_scan.cdr", std::ios::binary);
  ASSERT_TRUE(in.is_open());
  const std::vector<uint8_t> payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_EQ(payload.size(), 5824u);

  RosParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("sensor_msgs/msg/LaserScan", kLaserScanDef));
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(payload.data(), payload.size()), {}};
  auto rec = base->parseObject(1000, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  // Embedded Header stamp drives the record time when the option is on.
  ASSERT_TRUE(rec->ts.has_value());
  EXPECT_EQ(*rec->ts, 1'779'975'681'103'199'244LL);

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->frame_id, "base_link");
  EXPECT_EQ(pc->timestamp_ns, 1'779'975'681'103'199'244LL);
  // 1440 rays; 1041 are finite and inside [0.3, 40.0].
  EXPECT_EQ(pc->width, 1041u);
  EXPECT_EQ(pc->point_step, 12u);  // intensities[] is empty in this scan
  EXPECT_EQ(pc->row_step, pc->width * 12u);
  ASSERT_EQ(pc->data.size(), static_cast<size_t>(pc->width) * 12u);
  EXPECT_TRUE(pc->is_dense);

  // First kept ray is wire index 1: range 8.182730674743652 at
  // theta = angle_min + 1 * angle_increment (float wire values widened).
  const double kAngleMin = -3.1415927410125732;
  const double kAngleInc = 0.0043633198365569115;
  const double theta = kAngleMin + kAngleInc;
  EXPECT_NEAR(laserCloudFloat(*pc, 0, 0), 8.182730674743652 * std::cos(theta), 1e-4);
  EXPECT_NEAR(laserCloudFloat(*pc, 0, 4), 8.182730674743652 * std::sin(theta), 1e-4);

  // Every emitted point honors the wire's range bounds (dense output).
  for (uint32_t i = 0; i < pc->width; ++i) {
    const float x = laserCloudFloat(*pc, i, 0);
    const float y = laserCloudFloat(*pc, i, 4);
    const double r = std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y);
    ASSERT_TRUE(std::isfinite(r));
    ASSERT_GE(r, 0.29);
    ASSERT_LE(r, 40.01);
  }
}

}  // namespace
