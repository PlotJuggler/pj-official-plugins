#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>
#include <gtest/gtest.h>

#include <any>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "../foxglove_grid_codec.hpp"
#include "../foxglove_object_codecs.hpp"
#include "../foxglove_pointcloud_codec.hpp"
#include "../foxglove_voxelgrid_codec.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/grid_map_codec.hpp"
#include "pj_base/builtin/video_frame_codec.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

#ifndef PJ_PROTOBUF_PARSER_PLUGIN_PATH
#error "PJ_PROTOBUF_PARSER_PLUGIN_PATH must be defined"
#endif

namespace gp = google::protobuf;

namespace {

// Build a FileDescriptorSet containing a simple message:
//   message SensorData {
//     double temperature = 1;
//     int32 status = 2;
//   }
std::string buildSimpleSchema() {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test.proto");
  file_proto.set_syntax("proto3");

  auto* msg = file_proto.add_message_type();
  msg->set_name("SensorData");

  auto* f1 = msg->add_field();
  f1->set_name("temperature");
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* f2 = msg->add_field();
  f2->set_name("status");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;

  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

// Build a schema with a nested message:
//   message Header { int32 seq = 1; }
//   message Stamped { Header header = 1; double value = 2; }
std::string buildNestedSchema() {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_nested.proto");
  file_proto.set_syntax("proto3");

  // Header message
  auto* header_msg = file_proto.add_message_type();
  header_msg->set_name("Header");
  auto* seq_field = header_msg->add_field();
  seq_field->set_name("seq");
  seq_field->set_number(1);
  seq_field->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  seq_field->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  // Stamped message
  auto* stamped_msg = file_proto.add_message_type();
  stamped_msg->set_name("Stamped");
  auto* hdr_field = stamped_msg->add_field();
  hdr_field->set_name("header");
  hdr_field->set_number(1);
  hdr_field->set_type(gp::FieldDescriptorProto::TYPE_MESSAGE);
  hdr_field->set_type_name("Header");
  hdr_field->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* val_field = stamped_msg->add_field();
  val_field->set_name("value");
  val_field->set_number(2);
  val_field->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  val_field->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;

  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

// Build a schema with repeated field:
//   message RepeatedData { repeated double values = 1; int32 count = 2; }
std::string buildRepeatedSchema() {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_repeated.proto");
  file_proto.set_syntax("proto3");

  auto* msg = file_proto.add_message_type();
  msg->set_name("RepeatedData");

  auto* f1 = msg->add_field();
  f1->set_name("values");
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_REPEATED);

  auto* f2 = msg->add_field();
  f2->set_name("count");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;

  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

// Build a schema with an enum:
//   enum Color { RED = 0; GREEN = 1; BLUE = 2; }
//   message WithEnum { Color color = 1; double value = 2; }
std::string buildEnumSchema() {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_enum.proto");
  file_proto.set_syntax("proto3");

  auto* enum_type = file_proto.add_enum_type();
  enum_type->set_name("Color");
  auto* v0 = enum_type->add_value();
  v0->set_name("RED");
  v0->set_number(0);
  auto* v1 = enum_type->add_value();
  v1->set_name("GREEN");
  v1->set_number(1);
  auto* v2 = enum_type->add_value();
  v2->set_name("BLUE");
  v2->set_number(2);

  auto* msg = file_proto.add_message_type();
  msg->set_name("WithEnum");

  auto* f1 = msg->add_field();
  f1->set_name("color");
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_ENUM);
  f1->set_type_name("Color");
  f1->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* f2 = msg->add_field();
  f2->set_name("value");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;

  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

struct ProtobufParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_PROTOBUF_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(recorder.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  bool bindSchema(std::string_view type_name, const std::string& schema_bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(schema_bytes.data());
    return handle.bindSchema(type_name, PJ::Span<const uint8_t>(data, schema_bytes.size())).has_value();
  }

  bool parse(const std::string& serialized, int64_t ts = 1000) {
    const auto* data = reinterpret_cast<const uint8_t*>(serialized.data());
    return handle.parse(ts, PJ::Span<const uint8_t>(data, serialized.size())).has_value();
  }
};

// Helper: create a serialized message using DynamicMessage from our pool.
std::string serializeSimple(double temperature, int32_t status) {
  // Build pool and serialize
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test.proto");
  file_proto.set_syntax("proto3");

  auto* msg_desc = file_proto.add_message_type();
  msg_desc->set_name("SensorData");

  auto* f1 = msg_desc->add_field();
  f1->set_name("temperature");
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* f2 = msg_desc->add_field();
  f2->set_name("status");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* file_desc = pool.BuildFile(file_proto);
  const gp::Descriptor* descriptor = file_desc->FindMessageTypeByName("SensorData");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(descriptor)->New());
  const gp::Reflection* ref = msg->GetReflection();
  ref->SetDouble(msg.get(), descriptor->FindFieldByName("temperature"), temperature);
  ref->SetInt32(msg.get(), descriptor->FindFieldByName("status"), status);

  std::string out;
  msg->SerializeToString(&out);
  return out;
}

TEST(ProtobufParserTest, SimpleMessage) {
  ProtobufParserFixture f;
  f.setUp();

  auto schema = buildSimpleSchema();
  ASSERT_TRUE(f.bindSchema("SensorData", schema));

  auto payload = serializeSimple(23.5, 42);
  ASSERT_TRUE(f.parse(payload));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 2u);

  bool found_temp = false;
  bool found_status = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "temperature") {
      EXPECT_DOUBLE_EQ(field.numeric, 23.5);
      found_temp = true;
    } else if (field.name == "status") {
      EXPECT_DOUBLE_EQ(field.numeric, 42.0);
      found_status = true;
    }
  }
  EXPECT_TRUE(found_temp);
  EXPECT_TRUE(found_status);
}

TEST(ProtobufParserTest, NestedMessage) {
  ProtobufParserFixture f;
  f.setUp();

  auto schema = buildNestedSchema();
  ASSERT_TRUE(f.bindSchema("Stamped", schema));

  // Build serialized Stamped message with header.seq=7, value=3.14
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_nested.proto");
  file_proto.set_syntax("proto3");

  auto* hdr = file_proto.add_message_type();
  hdr->set_name("Header");
  auto* sf = hdr->add_field();
  sf->set_name("seq");
  sf->set_number(1);
  sf->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  sf->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* stamped = file_proto.add_message_type();
  stamped->set_name("Stamped");
  auto* hf = stamped->add_field();
  hf->set_name("header");
  hf->set_number(1);
  hf->set_type(gp::FieldDescriptorProto::TYPE_MESSAGE);
  hf->set_type_name("Header");
  hf->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);
  auto* vf = stamped->add_field();
  vf->set_name("value");
  vf->set_number(2);
  vf->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  vf->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file_proto);
  const gp::Descriptor* stamped_desc = fd->FindMessageTypeByName("Stamped");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(stamped_desc)->New());
  const gp::Reflection* ref = msg->GetReflection();

#pragma push_macro("GetMessage")
#undef GetMessage
  gp::Message* header_msg = ref->MutableMessage(msg.get(), stamped_desc->FindFieldByName("header"));
#pragma pop_macro("GetMessage")
  const gp::Reflection* hdr_ref = header_msg->GetReflection();
  hdr_ref->SetInt32(header_msg, header_msg->GetDescriptor()->FindFieldByName("seq"), 7);
  ref->SetDouble(msg.get(), stamped_desc->FindFieldByName("value"), 3.14);

  std::string payload;
  msg->SerializeToString(&payload);

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  bool found_seq = false;
  bool found_val = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "header/seq") {
      EXPECT_DOUBLE_EQ(field.numeric, 7.0);
      found_seq = true;
    } else if (field.name == "value") {
      EXPECT_DOUBLE_EQ(field.numeric, 3.14);
      found_val = true;
    }
  }
  EXPECT_TRUE(found_seq);
  EXPECT_TRUE(found_val);
}

TEST(ProtobufParserTest, RepeatedField) {
  ProtobufParserFixture f;
  f.setUp();

  auto schema = buildRepeatedSchema();
  ASSERT_TRUE(f.bindSchema("RepeatedData", schema));

  // Build serialized message with values=[1.0, 2.0, 3.0], count=3
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_repeated.proto");
  file_proto.set_syntax("proto3");

  auto* msg_desc = file_proto.add_message_type();
  msg_desc->set_name("RepeatedData");
  auto* f1 = msg_desc->add_field();
  f1->set_name("values");
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_REPEATED);
  auto* f2 = msg_desc->add_field();
  f2->set_name("count");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_INT32);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file_proto);
  const gp::Descriptor* desc = fd->FindMessageTypeByName("RepeatedData");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(desc)->New());
  const gp::Reflection* ref = msg->GetReflection();
  const gp::FieldDescriptor* vals = desc->FindFieldByName("values");
  ref->AddDouble(msg.get(), vals, 1.0);
  ref->AddDouble(msg.get(), vals, 2.0);
  ref->AddDouble(msg.get(), vals, 3.0);
  ref->SetInt32(msg.get(), desc->FindFieldByName("count"), 3);

  std::string payload;
  msg->SerializeToString(&payload);

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 4u);

  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "values[0]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 1.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[1].name, "values[1]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[1].numeric, 2.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[2].name, "values[2]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[2].numeric, 3.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[3].name, "count");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[3].numeric, 3.0);
}

TEST(ProtobufParserTest, EnumField) {
  ProtobufParserFixture f;
  f.setUp();

  auto schema = buildEnumSchema();
  ASSERT_TRUE(f.bindSchema("WithEnum", schema));

  // Build serialized message with color=GREEN(1), value=99.0
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("test_enum.proto");
  file_proto.set_syntax("proto3");

  auto* et = file_proto.add_enum_type();
  et->set_name("Color");
  auto* ev0 = et->add_value();
  ev0->set_name("RED");
  ev0->set_number(0);
  auto* ev1 = et->add_value();
  ev1->set_name("GREEN");
  ev1->set_number(1);
  auto* ev2 = et->add_value();
  ev2->set_name("BLUE");
  ev2->set_number(2);

  auto* msg_type = file_proto.add_message_type();
  msg_type->set_name("WithEnum");
  auto* cf = msg_type->add_field();
  cf->set_name("color");
  cf->set_number(1);
  cf->set_type(gp::FieldDescriptorProto::TYPE_ENUM);
  cf->set_type_name("Color");
  cf->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);
  auto* vf = msg_type->add_field();
  vf->set_name("value");
  vf->set_number(2);
  vf->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  vf->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file_proto);
  const gp::Descriptor* desc = fd->FindMessageTypeByName("WithEnum");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(desc)->New());
  const gp::Reflection* ref = msg->GetReflection();
  const gp::FieldDescriptor* color_fd = desc->FindFieldByName("color");
  const gp::EnumValueDescriptor* green = color_fd->enum_type()->FindValueByName("GREEN");
  ref->SetEnum(msg.get(), color_fd, green);
  ref->SetDouble(msg.get(), desc->FindFieldByName("value"), 99.0);

  std::string payload;
  msg->SerializeToString(&payload);

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  bool found_color = false;
  bool found_val = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "color") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kString);
      EXPECT_EQ(field.string_value, "GREEN");
      found_color = true;
    } else if (field.name == "value") {
      EXPECT_DOUBLE_EQ(field.numeric, 99.0);
      found_val = true;
    }
  }
  EXPECT_TRUE(found_color);
  EXPECT_TRUE(found_val);
}

TEST(ProtobufParserTest, ParseWithoutSchemFails) {
  ProtobufParserFixture f;
  f.setUp();
  // No bindSchema called — should fail.
  EXPECT_FALSE(f.parse("some bytes"));
}

TEST(ProtobufParserTest, InvalidSchemaFails) {
  ProtobufParserFixture f;
  f.setUp();
  std::string bad = "not a valid proto";
  EXPECT_FALSE(f.bindSchema("Foo", bad));
}

TEST(ProtobufParserTest, UnknownTypeFails) {
  ProtobufParserFixture f;
  f.setUp();
  auto schema = buildSimpleSchema();
  EXPECT_FALSE(f.bindSchema("NonExistent", schema));
}

TEST(ProtobufParserTest, TimestampPreserved) {
  ProtobufParserFixture f;
  f.setUp();
  auto schema = buildSimpleSchema();
  ASSERT_TRUE(f.bindSchema("SensorData", schema));
  auto payload = serializeSimple(1.0, 0);
  ASSERT_TRUE(f.parse(payload, 99999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 99999);
}

TEST(ProtobufParserTest, ManifestContainsEncoding) {
  ProtobufParserFixture f;
  f.setUp();
  // Manifest uses "encoding" as an array containing all supported encodings
  EXPECT_NE(f.handle.manifest().find("\"protobuf\""), std::string::npos);
}

// ---------------------------------------------------------------------------
// Embedded timestamp tests
// ---------------------------------------------------------------------------

// Build a schema with a top-level double "timestamp" field + a value field.
std::string buildTimestampSchema(
    const std::string& ts_field_name = "timestamp", const std::string& msg_name = "TsMessage") {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("ts_test_" + ts_field_name + ".proto");
  file_proto.set_syntax("proto3");

  auto* msg = file_proto.add_message_type();
  msg->set_name(msg_name);

  auto* f1 = msg->add_field();
  f1->set_name(ts_field_name);
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  auto* f2 = msg->add_field();
  f2->set_name("value");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;
  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

// Serialize a TsMessage using the FileDescriptorSet bytes from buildTimestampSchema.
// Avoids building a separate DescriptorPool that might collide with the global one.
std::string serializeTsMessage(
    const std::string& ts_field_name, double ts_seconds, double value, const std::string& msg_name = "TsMessage") {
  // Reconstruct the schema bytes the same way buildTimestampSchema does so we
  // can build a local pool and serialize without touching the global pool.
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("ts_serial_" + ts_field_name + "_" + msg_name + ".proto");
  file_proto.set_syntax("proto3");
  auto* msg_desc = file_proto.add_message_type();
  msg_desc->set_name(msg_name);
  auto* f1 = msg_desc->add_field();
  f1->set_name(ts_field_name);
  f1->set_number(1);
  f1->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f1->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);
  auto* f2 = msg_desc->add_field();
  f2->set_name("value");
  f2->set_number(2);
  f2->set_type(gp::FieldDescriptorProto::TYPE_DOUBLE);
  f2->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* file_desc = pool.BuildFile(file_proto);
  if (!file_desc) {
    return {};  // build failed — test will catch the empty payload
  }
  gp::DynamicMessageFactory factory(&pool);
  const auto* desc = pool.FindMessageTypeByName(msg_name);
  if (!desc) {
    return {};
  }
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(desc)->New());
  const auto* refl = msg->GetReflection();
  refl->SetDouble(msg.get(), desc->FindFieldByName(ts_field_name), ts_seconds);
  refl->SetDouble(msg.get(), desc->FindFieldByName("value"), value);
  std::string out;
  msg->SerializeToString(&out);
  return out;
}

TEST(ProtobufParserTest, EmbeddedTimestampDisabledByDefault) {
  ProtobufParserFixture f;
  f.setUp();
  auto schema = buildTimestampSchema("timestamp", "TsMsg1");
  ASSERT_TRUE(f.bindSchema("TsMsg1", schema));
  auto payload = serializeTsMessage("timestamp", 1234.567, 42.0, "TsMsg1");
  ASSERT_TRUE(f.parse(payload, 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 9999);  // host ts, not embedded
}

TEST(ProtobufParserTest, EmbeddedTimestampExplicitField) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true,"timestamp_field_name":"timestamp"})"));
  auto schema = buildTimestampSchema("timestamp", "TsMsg2");
  ASSERT_TRUE(f.bindSchema("TsMsg2", schema));
  auto payload = serializeTsMessage("timestamp", 1234.567, 42.0, "TsMsg2");
  ASSERT_TRUE(f.parse(payload, 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 1234567000000LL);
}

TEST(ProtobufParserTest, EmbeddedTimestampDefaultFallbackPicksTimestamp) {
  ProtobufParserFixture f;
  f.setUp();
  // No field name configured — should default to "timestamp"
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  auto schema = buildTimestampSchema("timestamp", "TsMsg3");
  ASSERT_TRUE(f.bindSchema("TsMsg3", schema));
  auto payload = serializeTsMessage("timestamp", 1234.567, 42.0, "TsMsg3");
  ASSERT_TRUE(f.parse(payload, 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 1234567000000LL);
}

TEST(ProtobufParserTest, EmbeddedTimestampDefaultFallbackPicksTs) {
  ProtobufParserFixture f;
  f.setUp();
  // No field name configured — should fall back to "ts" when "timestamp" absent
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  auto schema = buildTimestampSchema("ts", "TsMsg4");
  ASSERT_TRUE(f.bindSchema("TsMsg4", schema));
  auto payload = serializeTsMessage("ts", 1234.567, 42.0, "TsMsg4");
  ASSERT_TRUE(f.parse(payload, 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 1234567000000LL);
}

TEST(ProtobufParserTest, EmbeddedTimestampMissingFieldFallsBackToHost) {
  ProtobufParserFixture f;
  f.setUp();
  // Explicit field configured but not present in schema → host ts
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true,"timestamp_field_name":"timestamp"})"));
  auto schema = buildTimestampSchema("other_field", "TsMsg5");
  ASSERT_TRUE(f.bindSchema("TsMsg5", schema));
  auto payload = serializeTsMessage("other_field", 1234.567, 42.0, "TsMsg5");
  ASSERT_TRUE(f.parse(payload, 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 9999);
}

// ---------------------------------------------------------------------------
// Canonical VideoFrame fast path
//
// PJ.VideoFrame and foxglove.CompressedVideo are wire-identical, so a single
// decoder serves both. The bindSchema fast path bypasses the descriptor pool
// and registers a SchemaHandler that decodes the canonical wire bytes
// zero-copy. We feed serializeVideoFrame() output (the canonical writer) so the
// test stays in lock-step with the codec's wire layout.
// ---------------------------------------------------------------------------

namespace {

// A small, recognizable H.264-ish payload. The exact bytes are arbitrary — we
// only assert the decoder returns them verbatim and aliases the input buffer.
const std::vector<uint8_t> kVideoBlob = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xDE, 0xAD, 0xBE, 0xEF};

std::vector<uint8_t> buildVideoFrameWire(const std::string& format, PJ::Timestamp ts_ns) {
  PJ::sdk::VideoFrame frame;
  frame.timestamp_ns = ts_ns;
  frame.frame_id = "camera_optical";
  frame.format = format;
  frame.data = PJ::Span<const uint8_t>(kVideoBlob.data(), kVideoBlob.size());
  return PJ::serializeVideoFrame(frame);
}

// Decode via the in-process object route. The host calls parseObject() on the
// MessageParserPluginBase* directly (the C ABI vtable carries only the scalar
// parse() slot); context() hands back that base pointer — same pattern as the
// ros_parser object-route tests.
void checkVideoFrameObjectRoute(std::string_view registered_name) {
  ProtobufParserFixture f;
  f.setUp();

  // Empty schema bytes: the canonical fast path keys off the type name only.
  ASSERT_TRUE(f.bindSchema(registered_name, std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema(registered_name, empty_schema), PJ::sdk::BuiltinObjectType::kVideoFrame);

  const auto wire = buildVideoFrameWire("h264", 7'000'000'042LL);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  // Pass a real anchor so we can verify parse_object forwards it — the zero-copy
  // contract is that vf->data aliases the wire buffer, kept alive by this anchor.
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* vf = std::any_cast<PJ::sdk::VideoFrame>(&rec->object);
  ASSERT_NE(vf, nullptr);
  EXPECT_EQ(vf->frame_id, "camera_optical");
  EXPECT_EQ(vf->format, "h264");
  ASSERT_EQ(vf->data.size(), kVideoBlob.size());
  for (size_t i = 0; i < kVideoBlob.size(); ++i) {
    EXPECT_EQ(vf->data.data()[i], kVideoBlob[i]);
  }
  // Zero-copy: the decoded data span must alias the wire buffer we passed in,
  // not a fresh copy. The bytes live inside `wire` at the field-3 offset.
  EXPECT_GE(vf->data.data(), wire.data());
  EXPECT_LE(vf->data.data() + vf->data.size(), wire.data() + wire.size());
  // ...and the frame must carry the caller's anchor so those aliased bytes stay
  // alive for as long as a consumer holds the frame.
  EXPECT_EQ(vf->anchor, anchor) << "parse_object must forward payload.anchor (zero-copy lifetime token)";
}

}  // namespace

TEST(ProtobufParserTest, VideoFrameObjectRouteCanonicalName) {
  checkVideoFrameObjectRoute(PJ::kSchemaVideoFrame);
}

TEST(ProtobufParserTest, VideoFrameObjectRouteFoxgloveName) {
  // Same bytes, different registered schema name — one decoder serves both.
  checkVideoFrameObjectRoute("foxglove.CompressedVideo");
}

TEST(ProtobufParserTest, VideoFrameScalarRouteEmitsSlimMetadata) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(std::string(PJ::kSchemaVideoFrame), std::string{}));

  const auto wire = buildVideoFrameWire("h265", 7'000'000'042LL);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

  // …and the embedded stamp still reaches the datastore as its own series.
  const auto* timestamp = PJ::sdk::testing::ParserWriteRecorder::findField(row, "timestamp");
  ASSERT_NE(timestamp, nullptr);
  EXPECT_DOUBLE_EQ(timestamp->numeric, static_cast<double>(7'000'000'042LL) * 1e-9);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "camera_optical");

  const auto* format = PJ::sdk::testing::ParserWriteRecorder::findField(row, "format");
  ASSERT_NE(format, nullptr);
  EXPECT_EQ(format->string_value, "h265");

  const auto* data_size = PJ::sdk::testing::ParserWriteRecorder::findField(row, "data_size");
  ASSERT_NE(data_size, nullptr);
  EXPECT_DOUBLE_EQ(data_size->numeric, static_cast<double>(kVideoBlob.size()));
}

// ---------------------------------------------------------------------------
// foxglove.PointCloud → kPointCloud
//
// Mirrors how parser_ros promotes sensor_msgs/PointCloud2. We serialize a real
// foxglove.PointCloud message with the genuine protobuf serializer (so the wire
// layout — fixed32 stride/offset, nested timestamp/pose, repeated
// PackedElementField, bytes payload — matches what the decoder must read) and
// assert the canonical fields, the enum remap, and zero-copy aliasing.
// ---------------------------------------------------------------------------

namespace {

struct FoxgloveField {
  std::string name;
  uint32_t offset;
  int32_t numeric_type;  // foxglove PackedElementField.NumericType (UINT8=1, INT8=2, …)
};

// A recognizable 32-byte packed-point blob: 2 points * 16 bytes/point.
const std::vector<uint8_t> kCloudBlob = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                         0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                         0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

std::vector<uint8_t> buildFoxglovePointCloudWire(
    int64_t ts_sec, int32_t ts_nanos, const std::string& frame_id, uint32_t point_stride,
    const std::vector<FoxgloveField>& fields, const std::vector<uint8_t>& data, bool with_pose, double pose_tx) {
  gp::FileDescriptorProto file;
  file.set_name("foxglove_pc.proto");
  file.set_syntax("proto3");
  file.set_package("test");

  auto add_field = [](gp::DescriptorProto* m, const char* name, int num, gp::FieldDescriptorProto::Type type,
                      const char* type_name = nullptr, bool repeated = false) {
    auto* f = m->add_field();
    f->set_name(name);
    f->set_number(num);
    f->set_type(type);
    f->set_label(repeated ? gp::FieldDescriptorProto::LABEL_REPEATED : gp::FieldDescriptorProto::LABEL_OPTIONAL);
    if (type_name != nullptr) {
      f->set_type_name(type_name);
    }
  };

  auto* ts = file.add_message_type();
  ts->set_name("Timestamp");
  add_field(ts, "seconds", 1, gp::FieldDescriptorProto::TYPE_INT64);
  add_field(ts, "nanos", 2, gp::FieldDescriptorProto::TYPE_INT32);

  auto* vec = file.add_message_type();
  vec->set_name("Vec3");
  add_field(vec, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* quat = file.add_message_type();
  quat->set_name("Quat");
  add_field(quat, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "w", 4, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* pose = file.add_message_type();
  pose->set_name("Pose");
  add_field(pose, "position", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Vec3");
  add_field(pose, "orientation", 2, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Quat");

  auto* pef = file.add_message_type();
  pef->set_name("PackedElementField");
  add_field(pef, "name", 1, gp::FieldDescriptorProto::TYPE_STRING);
  add_field(pef, "offset", 2, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(pef, "type", 3, gp::FieldDescriptorProto::TYPE_INT32);  // enum on the wire == varint == int32

  auto* pc = file.add_message_type();
  pc->set_name("PointCloud");
  add_field(pc, "timestamp", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Timestamp");
  add_field(pc, "frame_id", 2, gp::FieldDescriptorProto::TYPE_STRING);
  add_field(pc, "pose", 3, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Pose");
  add_field(pc, "point_stride", 4, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(pc, "fields", 5, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.PackedElementField", true);
  add_field(pc, "data", 6, gp::FieldDescriptorProto::TYPE_BYTES);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file);
  const gp::Descriptor* pc_desc = fd->FindMessageTypeByName("PointCloud");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(pc_desc)->New());
  const gp::Reflection* ref = msg->GetReflection();

  gp::Message* tsm = ref->MutableMessage(msg.get(), pc_desc->FindFieldByName("timestamp"), &factory);
  const gp::Descriptor* tsd = tsm->GetDescriptor();
  tsm->GetReflection()->SetInt64(tsm, tsd->FindFieldByName("seconds"), ts_sec);
  tsm->GetReflection()->SetInt32(tsm, tsd->FindFieldByName("nanos"), ts_nanos);

  ref->SetString(msg.get(), pc_desc->FindFieldByName("frame_id"), frame_id);
  ref->SetUInt32(msg.get(), pc_desc->FindFieldByName("point_stride"), point_stride);
  ref->SetString(
      msg.get(), pc_desc->FindFieldByName("data"),
      std::string(reinterpret_cast<const char*>(data.data()), data.size()));

  if (with_pose) {
    gp::Message* pm = ref->MutableMessage(msg.get(), pc_desc->FindFieldByName("pose"), &factory);
    const gp::Descriptor* pd = pm->GetDescriptor();
    gp::Message* posm = pm->GetReflection()->MutableMessage(pm, pd->FindFieldByName("position"), &factory);
    const gp::Descriptor* vd = posm->GetDescriptor();
    posm->GetReflection()->SetDouble(posm, vd->FindFieldByName("x"), pose_tx);
  }

  const gp::FieldDescriptor* fields_f = pc_desc->FindFieldByName("fields");
  for (const auto& ff : fields) {
    gp::Message* fm = ref->AddMessage(msg.get(), fields_f, &factory);
    const gp::Descriptor* fdesc = fm->GetDescriptor();
    fm->GetReflection()->SetString(fm, fdesc->FindFieldByName("name"), ff.name);
    fm->GetReflection()->SetUInt32(fm, fdesc->FindFieldByName("offset"), ff.offset);
    fm->GetReflection()->SetInt32(fm, fdesc->FindFieldByName("type"), ff.numeric_type);
  }

  std::string out;
  msg->SerializeToString(&out);
  return std::vector<uint8_t>(out.begin(), out.end());
}

// Four channels spanning a few datatypes, including the signed/unsigned pair
// that the Foxglove↔SDK enum remap must keep straight (UINT8=1, INT8=2).
const std::vector<FoxgloveField> kCloudFields = {
    {"x", 0, 7},          // FLOAT32
    {"y", 4, 7},          // FLOAT32
    {"intensity", 8, 1},  // UINT8  → kUint8 (NOT kInt8)
    {"ring", 9, 2},       // INT8   → kInt8
};

// --- foxglove.VoxelGrid wire builder -------------------------------------
// Builds an authentic foxglove.VoxelGrid message (counts/strides are fixed32,
// cell_size/pose are nested messages) via DynamicMessageFactory, mirroring
// buildFoxglovePointCloudWire.
struct FoxgloveVoxelGridParams {
  int64_t ts_sec = 0;
  int32_t ts_nanos = 0;
  std::string frame_id;
  double origin_x = 0, origin_y = 0, origin_z = 0;  // pose.position
  double orient_z = 0;                              // pose.orientation.z (exercises the quaternion sub-parse)
  double cell_x = 0, cell_y = 0, cell_z = 0;        // cell_size
  uint32_t row_count = 0;
  uint32_t column_count = 0;
  uint32_t slice_stride = 0;
  uint32_t row_stride = 0;
  uint32_t cell_stride = 0;
  std::vector<FoxgloveField> fields;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> buildFoxgloveVoxelGridWire(const FoxgloveVoxelGridParams& p) {
  gp::FileDescriptorProto file;
  file.set_name("foxglove_vg.proto");
  file.set_syntax("proto3");
  file.set_package("test");

  auto add_field = [](gp::DescriptorProto* m, const char* name, int num, gp::FieldDescriptorProto::Type type,
                      const char* type_name = nullptr, bool repeated = false) {
    auto* f = m->add_field();
    f->set_name(name);
    f->set_number(num);
    f->set_type(type);
    f->set_label(repeated ? gp::FieldDescriptorProto::LABEL_REPEATED : gp::FieldDescriptorProto::LABEL_OPTIONAL);
    if (type_name != nullptr) {
      f->set_type_name(type_name);
    }
  };

  auto* ts = file.add_message_type();
  ts->set_name("Timestamp");
  add_field(ts, "seconds", 1, gp::FieldDescriptorProto::TYPE_INT64);
  add_field(ts, "nanos", 2, gp::FieldDescriptorProto::TYPE_INT32);

  auto* vec = file.add_message_type();
  vec->set_name("Vec3");
  add_field(vec, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* quat = file.add_message_type();
  quat->set_name("Quat");
  add_field(quat, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "w", 4, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* pose = file.add_message_type();
  pose->set_name("Pose");
  add_field(pose, "position", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Vec3");
  add_field(pose, "orientation", 2, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Quat");

  auto* pef = file.add_message_type();
  pef->set_name("PackedElementField");
  add_field(pef, "name", 1, gp::FieldDescriptorProto::TYPE_STRING);
  add_field(pef, "offset", 2, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(pef, "type", 3, gp::FieldDescriptorProto::TYPE_INT32);  // enum on the wire == varint == int32

  auto* vg = file.add_message_type();
  vg->set_name("VoxelGrid");
  add_field(vg, "timestamp", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Timestamp");
  add_field(vg, "frame_id", 2, gp::FieldDescriptorProto::TYPE_STRING);
  add_field(vg, "pose", 3, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Pose");
  add_field(vg, "row_count", 4, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(vg, "column_count", 5, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(vg, "cell_size", 6, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Vec3");
  add_field(vg, "slice_stride", 7, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(vg, "row_stride", 8, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(vg, "cell_stride", 9, gp::FieldDescriptorProto::TYPE_FIXED32);
  add_field(vg, "fields", 10, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.PackedElementField", true);
  add_field(vg, "data", 11, gp::FieldDescriptorProto::TYPE_BYTES);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file);
  const gp::Descriptor* vg_desc = fd->FindMessageTypeByName("VoxelGrid");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(vg_desc)->New());
  const gp::Reflection* ref = msg->GetReflection();

  gp::Message* tsm = ref->MutableMessage(msg.get(), vg_desc->FindFieldByName("timestamp"), &factory);
  const gp::Descriptor* tsd = tsm->GetDescriptor();
  tsm->GetReflection()->SetInt64(tsm, tsd->FindFieldByName("seconds"), p.ts_sec);
  tsm->GetReflection()->SetInt32(tsm, tsd->FindFieldByName("nanos"), p.ts_nanos);

  ref->SetString(msg.get(), vg_desc->FindFieldByName("frame_id"), p.frame_id);
  ref->SetUInt32(msg.get(), vg_desc->FindFieldByName("row_count"), p.row_count);
  ref->SetUInt32(msg.get(), vg_desc->FindFieldByName("column_count"), p.column_count);
  ref->SetUInt32(msg.get(), vg_desc->FindFieldByName("slice_stride"), p.slice_stride);
  ref->SetUInt32(msg.get(), vg_desc->FindFieldByName("row_stride"), p.row_stride);
  ref->SetUInt32(msg.get(), vg_desc->FindFieldByName("cell_stride"), p.cell_stride);
  ref->SetString(
      msg.get(), vg_desc->FindFieldByName("data"),
      std::string(reinterpret_cast<const char*>(p.data.data()), p.data.size()));

  gp::Message* pm = ref->MutableMessage(msg.get(), vg_desc->FindFieldByName("pose"), &factory);
  const gp::Descriptor* pd = pm->GetDescriptor();
  gp::Message* posm = pm->GetReflection()->MutableMessage(pm, pd->FindFieldByName("position"), &factory);
  const gp::Descriptor* vd = posm->GetDescriptor();
  posm->GetReflection()->SetDouble(posm, vd->FindFieldByName("x"), p.origin_x);
  posm->GetReflection()->SetDouble(posm, vd->FindFieldByName("y"), p.origin_y);
  posm->GetReflection()->SetDouble(posm, vd->FindFieldByName("z"), p.origin_z);
  gp::Message* orm = pm->GetReflection()->MutableMessage(pm, pd->FindFieldByName("orientation"), &factory);
  const gp::Descriptor* qd = orm->GetDescriptor();
  orm->GetReflection()->SetDouble(orm, qd->FindFieldByName("z"), p.orient_z);

  gp::Message* cm = ref->MutableMessage(msg.get(), vg_desc->FindFieldByName("cell_size"), &factory);
  const gp::Descriptor* cd = cm->GetDescriptor();
  cm->GetReflection()->SetDouble(cm, cd->FindFieldByName("x"), p.cell_x);
  cm->GetReflection()->SetDouble(cm, cd->FindFieldByName("y"), p.cell_y);
  cm->GetReflection()->SetDouble(cm, cd->FindFieldByName("z"), p.cell_z);

  const gp::FieldDescriptor* fields_f = vg_desc->FindFieldByName("fields");
  for (const auto& ff : p.fields) {
    gp::Message* fm = ref->AddMessage(msg.get(), fields_f, &factory);
    const gp::Descriptor* fdesc = fm->GetDescriptor();
    fm->GetReflection()->SetString(fm, fdesc->FindFieldByName("name"), ff.name);
    fm->GetReflection()->SetUInt32(fm, fdesc->FindFieldByName("offset"), ff.offset);
    fm->GetReflection()->SetInt32(fm, fdesc->FindFieldByName("type"), ff.numeric_type);
  }

  std::string out;
  msg->SerializeToString(&out);
  return std::vector<uint8_t>(out.begin(), out.end());
}

// 2*2*2 voxels, one occupancy byte each (8 bytes), Z-Y-X order.
const std::vector<uint8_t> kVoxelData = {0, 100, 50, 0xFF, 10, 20, 30, 40};
const std::vector<FoxgloveField> kVoxelFields = {{"occupancy", 0, 1}};  // UINT8 -> kUint8

}  // namespace

TEST(ProtobufParserTest, FoxgloveVoxelGridCodecDecodesAndDerivesSliceCount) {
  FoxgloveVoxelGridParams p;
  p.ts_sec = 7;
  p.ts_nanos = 250;
  p.frame_id = "map";
  p.origin_x = 1.0;
  p.origin_y = 2.0;
  p.origin_z = 3.0;
  p.orient_z = 1.0;  // exercises the quaternion sub-parse
  p.cell_x = 0.1;
  p.cell_y = 0.2;
  p.cell_z = 0.3;
  p.row_count = 2;
  p.column_count = 2;
  p.cell_stride = 1;   // one occupancy byte per voxel
  p.row_stride = 2;    // column_count * cell_stride
  p.slice_stride = 4;  // row_count * row_stride
  p.fields = kVoxelFields;
  p.data = kVoxelData;
  const auto wire = buildFoxgloveVoxelGridWire(p);

  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto decoded = pj_protobuf::deserializeFoxgloveVoxelGridView(wire.data(), wire.size(), anchor);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const auto& grid = *decoded;

  EXPECT_EQ(grid.frame_id, "map");
  EXPECT_EQ(grid.timestamp_ns, 7'000'000'250LL);  // 7s + 250ns
  EXPECT_DOUBLE_EQ(grid.origin.position.x, 1.0);
  EXPECT_DOUBLE_EQ(grid.origin.position.y, 2.0);
  EXPECT_DOUBLE_EQ(grid.origin.position.z, 3.0);
  EXPECT_DOUBLE_EQ(grid.origin.orientation.z, 1.0);
  EXPECT_DOUBLE_EQ(grid.origin.orientation.w, 1.0);  // w omitted on the wire -> identity default
  EXPECT_DOUBLE_EQ(grid.cell_size.x, 0.1);
  EXPECT_DOUBLE_EQ(grid.cell_size.y, 0.2);
  EXPECT_DOUBLE_EQ(grid.cell_size.z, 0.3);
  EXPECT_EQ(grid.row_count, 2u);
  EXPECT_EQ(grid.column_count, 2u);
  EXPECT_EQ(grid.cell_stride, 1u);
  EXPECT_EQ(grid.row_stride, 2u);
  EXPECT_EQ(grid.slice_stride, 4u);
  EXPECT_EQ(grid.slice_count, 2u);  // derived: data.size()(8) / slice_stride(4)

  ASSERT_EQ(grid.fields.size(), 1u);
  EXPECT_EQ(grid.fields[0].name, "occupancy");
  EXPECT_EQ(grid.fields[0].offset, 0u);
  EXPECT_EQ(grid.fields[0].datatype, PJ::sdk::PointField::Datatype::kUint8);
  EXPECT_EQ(grid.fields[0].count, 1u);

  // Zero-copy: the voxel span must alias the wire buffer, not a copy.
  ASSERT_EQ(grid.data.size(), kVoxelData.size());
  EXPECT_GE(grid.data.data(), wire.data());
  EXPECT_LE(grid.data.data() + grid.data.size(), wire.data() + wire.size());
  EXPECT_EQ(grid.anchor, anchor);
}

TEST(ProtobufParserTest, FoxgloveVoxelGridObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  // Empty schema bytes: the canonical fast path keys off the type name only.
  ASSERT_TRUE(f.bindSchema("foxglove.VoxelGrid", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.VoxelGrid", empty_schema), PJ::sdk::BuiltinObjectType::kVoxelGrid);

  FoxgloveVoxelGridParams p;
  p.frame_id = "map";
  p.cell_x = 0.05;
  p.cell_y = 0.05;
  p.cell_z = 0.05;
  p.row_count = 2;
  p.column_count = 2;
  p.cell_stride = 1;
  p.row_stride = 2;
  p.slice_stride = 4;
  p.fields = kVoxelFields;
  p.data = kVoxelData;
  const auto wire = buildFoxgloveVoxelGridWire(p);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* vg = std::any_cast<PJ::sdk::VoxelGrid>(&rec->object);
  ASSERT_NE(vg, nullptr);
  EXPECT_EQ(vg->frame_id, "map");
  EXPECT_EQ(vg->column_count, 2u);
  EXPECT_EQ(vg->row_count, 2u);
  EXPECT_EQ(vg->slice_count, 2u);
  ASSERT_EQ(vg->fields.size(), 1u);
  EXPECT_EQ(vg->fields[0].datatype, PJ::sdk::PointField::Datatype::kUint8);
  // Zero-copy + anchor forwarding across the in-process object route.
  EXPECT_GE(vg->data.data(), wire.data());
  EXPECT_LE(vg->data.data() + vg->data.size(), wire.data() + wire.size());
  EXPECT_EQ(vg->anchor, anchor);
}

TEST(ProtobufParserTest, FoxglovePointCloudCodecDecodesAndSynthesizes) {
  const auto wire = buildFoxglovePointCloudWire(7, 250, "lidar_top", 16, kCloudFields, kCloudBlob, false, 0.0);

  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto decoded = pj_protobuf::deserializeFoxglovePointCloudView(wire.data(), wire.size(), anchor);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const auto& cloud = decoded->cloud;

  EXPECT_EQ(cloud.frame_id, "lidar_top");
  EXPECT_EQ(cloud.point_step, 16u);
  EXPECT_EQ(cloud.height, 1u);
  EXPECT_EQ(cloud.width, 2u);      // 32 bytes / 16 stride
  EXPECT_EQ(cloud.row_step, 32u);  // width * point_step
  EXPECT_FALSE(cloud.is_bigendian);
  EXPECT_TRUE(cloud.is_dense);
  EXPECT_EQ(cloud.timestamp_ns, 7'000'000'250LL);  // 7s + 250ns

  ASSERT_EQ(cloud.fields.size(), 4u);
  using DT = PJ::sdk::PointField::Datatype;
  EXPECT_EQ(cloud.fields[0].name, "x");
  EXPECT_EQ(cloud.fields[0].offset, 0u);
  EXPECT_EQ(cloud.fields[0].datatype, DT::kFloat32);
  EXPECT_EQ(cloud.fields[0].count, 1u);
  // The crux: Foxglove's swapped enum must map UINT8(1)→kUint8 and INT8(2)→kInt8.
  EXPECT_EQ(cloud.fields[2].datatype, DT::kUint8);
  EXPECT_EQ(cloud.fields[2].offset, 8u);
  EXPECT_EQ(cloud.fields[3].datatype, DT::kInt8);
  EXPECT_EQ(cloud.fields[3].offset, 9u);

  // Zero-copy: the packed-point span must alias the wire buffer, not a copy.
  ASSERT_EQ(cloud.data.size(), kCloudBlob.size());
  EXPECT_GE(cloud.data.data(), wire.data());
  EXPECT_LE(cloud.data.data() + cloud.data.size(), wire.data() + wire.size());
  EXPECT_EQ(cloud.anchor, anchor);
  EXPECT_FALSE(decoded->has_pose);
}

// Foxglove stores colour as four separate uint8 channels (red/green/blue/alpha).
// They describe one per-point colour, so the codec normalizes them to a single
// canonical packed 'rgba' uint32 field (R,G,B,A increasing-address) — metadata only,
// the bytes are already in canonical order, so the zero-copy span is untouched.
const std::vector<FoxgloveField> kColorCloudFields = {{"x", 0, 7},          {"y", 4, 7},     {"z", 8, 7},
                                                      {"intensity", 12, 7}, {"red", 16, 1},  {"green", 17, 1},
                                                      {"blue", 18, 1},      {"alpha", 19, 1}};

TEST(ProtobufParserTest, FoxglovePointCloudColorChannelsCollapseToRgba) {
  const std::vector<uint8_t> blob(40, 0);  // 2 points * 20-byte stride
  const auto wire = buildFoxglovePointCloudWire(0, 0, "lidar", 20, kColorCloudFields, blob, false, 0.0);

  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto decoded = pj_protobuf::deserializeFoxglovePointCloudView(wire.data(), wire.size(), anchor);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const auto& cloud = decoded->cloud;
  using DT = PJ::sdk::PointField::Datatype;

  // x, y, z, intensity, rgba — the 4 colour channels became one packed field.
  ASSERT_EQ(cloud.fields.size(), 5u);
  EXPECT_EQ(cloud.fields[0].name, "x");
  EXPECT_EQ(cloud.fields[3].name, "intensity");
  EXPECT_EQ(cloud.fields[4].name, "rgba");
  EXPECT_EQ(cloud.fields[4].offset, 16u);  // = the red channel's offset (bytes unchanged)
  EXPECT_EQ(cloud.fields[4].datatype, DT::kUint32);
  EXPECT_EQ(cloud.fields[4].count, 1u);

  // Zero-copy is preserved: collapsing is a pure metadata rewrite.
  EXPECT_EQ(cloud.point_step, 20u);
  EXPECT_GE(cloud.data.data(), wire.data());
}

TEST(ProtobufParserTest, FoxglovePointCloudCodecFlagsNonIdentityPose) {
  const auto wire = buildFoxglovePointCloudWire(0, 0, "lidar", 16, kCloudFields, kCloudBlob, true, 3.5);
  auto decoded = pj_protobuf::deserializeFoxglovePointCloudView(wire.data(), wire.size(), nullptr);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_TRUE(decoded->has_pose);
  EXPECT_FALSE(decoded->pose_is_identity);  // translation x = 3.5 → not identity
}

TEST(ProtobufParserTest, FoxglovePointCloudObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  // Empty schema bytes: the canonical fast path keys off the type name only.
  ASSERT_TRUE(f.bindSchema("foxglove.PointCloud", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.PointCloud", empty_schema), PJ::sdk::BuiltinObjectType::kPointCloud);

  const auto wire = buildFoxglovePointCloudWire(7, 0, "lidar_top", 16, kCloudFields, kCloudBlob, false, 0.0);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->frame_id, "lidar_top");
  EXPECT_EQ(pc->point_step, 16u);
  EXPECT_EQ(pc->width, 2u);
  ASSERT_EQ(pc->fields.size(), 4u);
  EXPECT_EQ(pc->fields[2].datatype, PJ::sdk::PointField::Datatype::kUint8);
  // Zero-copy + anchor forwarding across the in-process object route.
  EXPECT_GE(pc->data.data(), wire.data());
  EXPECT_LE(pc->data.data() + pc->data.size(), wire.data() + wire.size());
  EXPECT_EQ(pc->anchor, anchor);
}

TEST(ProtobufParserTest, FoxglovePointCloudScalarRoute) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.PointCloud", std::string{}));

  const auto wire = buildFoxglovePointCloudWire(7, 250, "lidar_top", 16, kCloudFields, kCloudBlob, false, 0.0);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

  // The embedded sensor stamp is emitted as a series regardless: the row is
  // filed under the host clock (555), so without this column the message's own
  // time would be unplottable.
  const auto* timestamp = PJ::sdk::testing::ParserWriteRecorder::findField(row, "timestamp");
  ASSERT_NE(timestamp, nullptr);
  EXPECT_DOUBLE_EQ(timestamp->numeric, static_cast<double>(7'000'000'250LL) * 1e-9);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "lidar_top");

  const auto* point_count = PJ::sdk::testing::ParserWriteRecorder::findField(row, "point_count");
  ASSERT_NE(point_count, nullptr);
  EXPECT_DOUBLE_EQ(point_count->numeric, 2.0);

  // point_step / num_fields are cloud layout constants, not time series: they
  // describe how to read data[] and never move over a recording.
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(row, "point_step"), nullptr);
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(row, "num_fields"), nullptr);
  EXPECT_EQ(row.fields.size(), 3u);
}

}  // namespace

// ---------------------------------------------------------------------------
// Well-known Foxglove object decoders (foxglove_object_codecs.cpp)
//
// These 5 decoders had no protobuf-layer test. We build minimal but genuine
// Foxglove-wire protobuf bytes with a tiny hand-rolled writer (exact control
// over field numbers / wire types, matching the schemas the decoder reads) and
// assert the canonical field mapping. Happy-path coverage for the wire reader.
// ---------------------------------------------------------------------------

namespace {

// Minimal protobuf wire writer — just the field shapes these decoders consume.
struct PW {
  std::vector<uint8_t> b;
  void rawVarint(uint64_t v) {
    while (v >= 0x80) {
      b.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
      v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
  }
  void tag(int field, int wire) {
    rawVarint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wire));
  }
  void varint(int field, uint64_t v) {
    tag(field, 0);
    rawVarint(v);
  }
  void fixed32(int field, uint32_t v) {
    tag(field, 5);
    for (int i = 0; i < 4; ++i) {
      b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
  }
  void dbl(int field, double d) {
    tag(field, 1);
    const uint64_t bits = std::bit_cast<uint64_t>(d);
    for (int i = 0; i < 8; ++i) {
      b.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
  }
  void str(int field, const std::string& s) {
    tag(field, 2);
    rawVarint(s.size());
    b.insert(b.end(), s.begin(), s.end());
  }
  void bytesField(int field, const std::vector<uint8_t>& v) {
    tag(field, 2);
    rawVarint(v.size());
    b.insert(b.end(), v.begin(), v.end());
  }
  void sub(int field, const PW& w) {
    tag(field, 2);
    rawVarint(w.b.size());
    b.insert(b.end(), w.b.begin(), w.b.end());
  }
  void packedDoubles(int field, const std::vector<double>& ds) {
    PW inner;
    for (double d : ds) {
      const uint64_t bits = std::bit_cast<uint64_t>(d);
      for (int i = 0; i < 8; ++i) {
        inner.b.push_back(static_cast<uint8_t>(bits >> (8 * i)));
      }
    }
    sub(field, inner);
  }
};

// google.protobuf.Timestamp { seconds=1, nanos=2 }.
PW foxgloveTimestamp(int64_t seconds, int32_t nanos) {
  PW ts;
  ts.varint(1, static_cast<uint64_t>(seconds));
  ts.varint(2, static_cast<uint64_t>(static_cast<uint32_t>(nanos)));
  return ts;
}

}  // namespace

TEST(ProtobufParserTest, FoxgloveCompressedImageDecodes) {
  const std::vector<uint8_t> blob = {0xFF, 0xD8, 0xFF, 0xE0, 0xDE, 0xAD};  // jpeg-ish
  PW img;
  img.sub(1, foxgloveTimestamp(7, 42));
  img.bytesField(2, blob);
  img.str(3, "jpeg");            // foxglove `format` -> sdk `encoding`
  img.str(4, "camera_optical");  // frame_id -> sdk::Image.frame_id
  const auto& w = img.b;

  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto r = pj_protobuf::deserializeFoxgloveCompressedImageView(w.data(), w.size(), anchor);
  ASSERT_TRUE(r) << r.error();
  EXPECT_EQ(r->encoding, "jpeg");
  EXPECT_EQ(r->frame_id, "camera_optical");
  EXPECT_EQ(r->timestamp_ns, 7'000'000'042LL);
  ASSERT_EQ(r->data.size(), blob.size());
  for (size_t i = 0; i < blob.size(); ++i) {
    EXPECT_EQ(r->data.data()[i], blob[i]);
  }
  // Zero-copy: the data span aliases the input buffer.
  EXPECT_GE(r->data.data(), w.data());
  EXPECT_LE(r->data.data() + r->data.size(), w.data() + w.size());
}

TEST(ProtobufParserTest, FoxgloveRawImageDecodes) {
  const std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 2x2 rgb8
  PW img;
  img.sub(1, foxgloveTimestamp(5, 0));
  img.str(2, "camera_optical");  // frame_id
  img.fixed32(3, 2);             // width
  img.fixed32(4, 2);             // height
  img.str(5, "rgb8");            // encoding (verbatim — drives the consumer's raw renderer)
  img.fixed32(6, 6);             // step = width * 3 bytes
  img.bytesField(7, pixels);     // data
  const auto& w = img.b;

  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto r = pj_protobuf::deserializeFoxgloveRawImageView(w.data(), w.size(), anchor);
  ASSERT_TRUE(r) << r.error();
  EXPECT_EQ(r->frame_id, "camera_optical");
  EXPECT_EQ(r->encoding, "rgb8");
  EXPECT_EQ(r->width, 2u);
  EXPECT_EQ(r->height, 2u);
  EXPECT_EQ(r->row_step, 6u);
  EXPECT_EQ(r->timestamp_ns, 5'000'000'000LL);
  ASSERT_EQ(r->data.size(), pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i) {
    EXPECT_EQ(r->data.data()[i], pixels[i]);
  }
  // Zero-copy: the data span aliases the input buffer.
  EXPECT_GE(r->data.data(), w.data());
  EXPECT_LE(r->data.data() + r->data.size(), w.data() + w.size());
}

namespace {

// Build a FileDescriptorSet for foxglove.RawImage with caller-supplied field
// numbers. Field TYPES are irrelevant to descriptor-driven resolution (it maps
// field NAME -> number) and to the hand-rolled decoder (which reads the wire by
// resolved number), so we use simple scalar types and avoid pulling in the
// google.protobuf.Timestamp dependency. This lets a test mimic a self-describing
// .mcap whose embedded foxglove.RawImage uses NON-OFFICIAL field numbering.
std::string buildRawImageSchema(int width, int height, int encoding, int step, int data, int frame_id) {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("foxglove/RawImage.proto");
  file_proto.set_package("foxglove");
  file_proto.set_syntax("proto3");
  auto* msg = file_proto.add_message_type();
  msg->set_name("RawImage");
  auto add = [&](const char* name, int number, gp::FieldDescriptorProto::Type type) {
    auto* f = msg->add_field();
    f->set_name(name);
    f->set_number(number);
    f->set_type(type);
    f->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);
  };
  add("width", width, gp::FieldDescriptorProto::TYPE_FIXED32);
  add("height", height, gp::FieldDescriptorProto::TYPE_FIXED32);
  add("encoding", encoding, gp::FieldDescriptorProto::TYPE_STRING);
  add("step", step, gp::FieldDescriptorProto::TYPE_FIXED32);
  add("data", data, gp::FieldDescriptorProto::TYPE_BYTES);
  add("frame_id", frame_id, gp::FieldDescriptorProto::TYPE_STRING);

  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;
  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

}  // namespace

// Regression: a self-describing .mcap may embed a foxglove.RawImage schema whose
// field NUMBERS differ from the official Foxglove numbering (observed in real
// DROID-dataset files written by mcap-rs: width=2, height=3, encoding=4, step=5,
// data=6, frame_id=7 — i.e. frame_id moved to the end, everything else shifted
// down). The decoder must honor the embedded descriptor's field numbers, not the
// hardcoded official ones, or every image fails to parse and renders as a blank
// (placeholder-texture) tile.
TEST(ProtobufParserTest, FoxgloveRawImageHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();

  // The DROID-file numbering, distinct from the official width=3..data=7.
  const std::string schema = buildRawImageSchema(
      /*width=*/2, /*height=*/3, /*encoding=*/4,
      /*step=*/5, /*data=*/6, /*frame_id=*/7);
  ASSERT_TRUE(f.bindSchema("foxglove.RawImage", schema));

  const std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 2x2 rgb8
  PW img;
  img.sub(1, foxgloveTimestamp(5, 0));  // timestamp stays field 1 in both numberings
  img.fixed32(2, 2);                    // width   (variant number)
  img.fixed32(3, 2);                    // height  (variant number)
  img.str(4, "rgb8");                   // encoding(variant number)
  img.fixed32(5, 6);                    // step    (variant number)
  img.bytesField(6, pixels);            // data    (variant number)
  img.str(7, "camera_optical");         // frame_id(variant number)
  const auto& wire = img.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* im = std::any_cast<PJ::sdk::Image>(&rec->object);
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->width, 2u);
  EXPECT_EQ(im->height, 2u);
  EXPECT_EQ(im->row_step, 6u);
  EXPECT_EQ(im->encoding, "rgb8");
  EXPECT_EQ(im->frame_id, "camera_optical");
  ASSERT_EQ(im->data.size(), pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i) {
    EXPECT_EQ(im->data.data()[i], pixels[i]);
  }
}

namespace {

// Build a FileDescriptorSet for a foxglove.<message_name> with caller-supplied
// (field name, number) pairs. Field TYPES are irrelevant to descriptor-driven
// resolution (it maps NAME -> number) and to the hand-rolled decoders, so we use
// scalar/bytes types and avoid pulling in message dependencies.
std::string buildFoxgloveSchema(
    const std::string& message_name, const std::vector<std::pair<std::string, int>>& fields) {
  gp::FileDescriptorProto file_proto;
  file_proto.set_name("foxglove/" + message_name + ".proto");
  file_proto.set_package("foxglove");
  file_proto.set_syntax("proto3");
  auto* msg = file_proto.add_message_type();
  msg->set_name(message_name);
  for (const auto& [fname, number] : fields) {
    auto* f = msg->add_field();
    f->set_name(fname);
    f->set_number(number);
    f->set_type(gp::FieldDescriptorProto::TYPE_BYTES);  // type is irrelevant to resolution
    f->set_label(gp::FieldDescriptorProto::LABEL_OPTIONAL);
  }
  gp::FileDescriptorSet fd_set;
  *fd_set.add_file() = file_proto;
  std::string out;
  fd_set.SerializeToString(&out);
  return out;
}

}  // namespace

// foxglove.CompressedImage with a NON-default field numbering embedded in the
// schema. The hardcoded numbering is { data=2, format=3, frame_id=4 }; here we
// shuffle to { format=2, frame_id=3, data=4 }. The decoder must follow the
// descriptor, not the hardcoded numbers.
TEST(ProtobufParserTest, FoxgloveCompressedImageHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema =
      buildFoxgloveSchema("CompressedImage", {{"timestamp", 1}, {"format", 2}, {"frame_id", 3}, {"data", 4}});
  ASSERT_TRUE(f.bindSchema("foxglove.CompressedImage", schema));

  const std::vector<uint8_t> blob = {0xFF, 0xD8, 0xFF, 0xE0, 0xDE, 0xAD};
  PW img;
  img.sub(1, foxgloveTimestamp(7, 42));
  img.str(2, "jpeg");  // format (variant number)
  img.str(3, "cam");   // frame_id (variant number)
  img.bytesField(4, blob);
  const auto& wire = img.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* im = std::any_cast<PJ::sdk::Image>(&rec->object);
  ASSERT_NE(im, nullptr);
  EXPECT_EQ(im->encoding, "jpeg");
  EXPECT_EQ(im->frame_id, "cam");
  ASSERT_EQ(im->data.size(), blob.size());
  for (size_t i = 0; i < blob.size(); ++i) {
    EXPECT_EQ(im->data.data()[i], blob[i]);
  }
}

// foxglove.CameraCalibration: the codec's hardcoded numbering is the "frame_id
// last" variant { width=2..frame_id=9 }, so files using the OFFICIAL numbering
// { frame_id=2, width=3..P=9 } currently fail. The descriptor must drive it.
TEST(ProtobufParserTest, FoxgloveCameraCalibrationHonorsOfficialSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema = buildFoxgloveSchema(
      "CameraCalibration", {{"timestamp", 1},
                            {"frame_id", 2},
                            {"width", 3},
                            {"height", 4},
                            {"distortion_model", 5},
                            {"D", 6},
                            {"K", 7},
                            {"R", 8},
                            {"P", 9}});
  ASSERT_TRUE(f.bindSchema("foxglove.CameraCalibration", schema));

  PW cc;
  cc.sub(1, foxgloveTimestamp(0, 0));
  cc.str(2, "camera_optical");                           // frame_id (official number)
  cc.fixed32(3, 640);                                    // width    (official number)
  cc.fixed32(4, 480);                                    // height   (official number)
  cc.str(5, "plumb_bob");                                // distortion_model
  cc.packedDoubles(6, {0.1, 0.2, 0.3, 0.4, 0.5});        // D
  cc.packedDoubles(7, {1, 0, 320, 0, 1, 240, 0, 0, 1});  // K (3x3)
  const auto& wire = cc.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* ci = std::any_cast<PJ::sdk::CameraInfo>(&rec->object);
  ASSERT_NE(ci, nullptr);
  EXPECT_EQ(ci->width, 640u);
  EXPECT_EQ(ci->height, 480u);
  EXPECT_EQ(ci->distortion_model, "plumb_bob");
  EXPECT_EQ(ci->frame_id, "camera_optical");
  ASSERT_GE(ci->D.size(), 5u);
  EXPECT_DOUBLE_EQ(ci->D[0], 0.1);
  EXPECT_DOUBLE_EQ(ci->K[2], 320.0);
}

// foxglove.FrameTransform with a NON-default numbering: official is { parent=2,
// child=3, translation=4, rotation=5 }; here { translation=2, rotation=3,
// parent=4, child=5 }. The decoder must follow the descriptor.
TEST(ProtobufParserTest, FoxgloveFrameTransformHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema = buildFoxgloveSchema(
      "FrameTransform",
      {{"timestamp", 1}, {"translation", 2}, {"rotation", 3}, {"parent_frame_id", 4}, {"child_frame_id", 5}});
  ASSERT_TRUE(f.bindSchema("foxglove.FrameTransform", schema));

  PW vec3;
  vec3.dbl(1, 1.5);
  vec3.dbl(2, 2.5);
  vec3.dbl(3, 3.5);
  PW quat;
  quat.dbl(1, 0.0);
  quat.dbl(2, 0.0);
  quat.dbl(3, 0.0);
  quat.dbl(4, 1.0);
  PW tf;
  tf.sub(1, foxgloveTimestamp(9, 0));
  tf.sub(2, vec3);         // translation (variant number)
  tf.sub(3, quat);         // rotation (variant number)
  tf.str(4, "world");      // parent_frame_id (variant number)
  tf.str(5, "base_link");  // child_frame_id (variant number)
  const auto& wire = tf.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* tfs = std::any_cast<PJ::sdk::FrameTransforms>(&rec->object);
  ASSERT_NE(tfs, nullptr);
  ASSERT_EQ(tfs->transforms.size(), 1u);
  EXPECT_EQ(tfs->transforms[0].parent_frame_id, "world");
  EXPECT_EQ(tfs->transforms[0].child_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(tfs->transforms[0].translation.x, 1.5);
  EXPECT_DOUBLE_EQ(tfs->transforms[0].translation.z, 3.5);
}

// foxglove.LaserScan with frame_id moved to the end (official is field 2). The
// object route projects to a kPointCloud; the resolved frame_id must survive.
TEST(ProtobufParserTest, FoxgloveLaserScanHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema = buildFoxgloveSchema(
      "LaserScan", {{"timestamp", 1},
                    {"pose", 2},
                    {"start_angle", 3},
                    {"end_angle", 4},
                    {"ranges", 5},
                    {"intensities", 6},
                    {"frame_id", 7}});
  ASSERT_TRUE(f.bindSchema("foxglove.LaserScan", schema));

  PW ls;
  ls.sub(1, foxgloveTimestamp(0, 0));
  ls.dbl(3, 0.0);                   // start_angle (variant number)
  ls.dbl(4, 1.0);                   // end_angle   (variant number)
  ls.packedDoubles(5, {1.0, 2.0});  // ranges      (variant number)
  ls.str(7, "laser");               // frame_id    (variant number)
  const auto& wire = ls.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->frame_id, "laser");
  EXPECT_GT(pc->width, 0u);  // rays projected to points
}

// foxglove.ImageAnnotations with the top-level oneof-of-arrays shuffled
// (official circles=1, points=2, texts=3). Nested annotations stay official.
TEST(ProtobufParserTest, FoxgloveImageAnnotationsHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema = buildFoxgloveSchema("ImageAnnotations", {{"circles", 3}, {"points", 1}, {"texts", 2}});
  ASSERT_TRUE(f.bindSchema("foxglove.ImageAnnotations", schema));

  PW circle;  // minimal sub-annotations; empty bodies decode to defaults
  PW points_ann;
  PW text;
  PW ann;
  ann.sub(3, circle);      // circles (variant number)
  ann.sub(1, points_ann);  // points  (variant number)
  ann.sub(2, text);        // texts   (variant number)
  const auto& wire = ann.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* a = std::any_cast<PJ::sdk::ImageAnnotations>(&rec->object);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->circles.size(), 1u);
  EXPECT_EQ(a->points.size(), 1u);
  EXPECT_EQ(a->texts.size(), 1u);
}

namespace {

void addProtoField(
    gp::DescriptorProto* m, const char* name, int num, gp::FieldDescriptorProto::Type type,
    const char* type_name = nullptr, bool repeated = false) {
  auto* fld = m->add_field();
  fld->set_name(name);
  fld->set_number(num);
  fld->set_type(type);
  fld->set_label(repeated ? gp::FieldDescriptorProto::LABEL_REPEATED : gp::FieldDescriptorProto::LABEL_OPTIONAL);
  if (type_name != nullptr) {
    fld->set_type_name(type_name);
  }
}

// foxglove.PointCloud with frame_id moved to the end AND the nested
// PackedElementField renumbered, exercising both top-level and nested resolution.
std::string buildPointCloudVariantSchema() {
  gp::FileDescriptorProto file;
  file.set_name("foxglove/PointCloud.proto");
  file.set_package("foxglove");
  file.set_syntax("proto3");
  auto* pef = file.add_message_type();
  pef->set_name("PackedElementField");
  addProtoField(pef, "name", 3, gp::FieldDescriptorProto::TYPE_STRING);     // renumbered
  addProtoField(pef, "offset", 1, gp::FieldDescriptorProto::TYPE_FIXED32);  // renumbered
  addProtoField(pef, "type", 2, gp::FieldDescriptorProto::TYPE_INT32);      // renumbered
  auto* pc = file.add_message_type();
  pc->set_name("PointCloud");
  addProtoField(pc, "timestamp", 1, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(pc, "pose", 2, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(pc, "point_stride", 3, gp::FieldDescriptorProto::TYPE_FIXED32);
  addProtoField(pc, "fields", 4, gp::FieldDescriptorProto::TYPE_MESSAGE, ".foxglove.PackedElementField", true);
  addProtoField(pc, "data", 5, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(pc, "frame_id", 6, gp::FieldDescriptorProto::TYPE_STRING);  // moved to the end
  gp::FileDescriptorSet fds;
  *fds.add_file() = file;
  std::string out;
  fds.SerializeToString(&out);
  return out;
}

// foxglove.SceneUpdate whose nested SceneEntity has frame_id renumbered (the real
// failure mode: SceneEntity carries frame_id, so renumbering files renumber it).
std::string buildSceneUpdateVariantSchema() {
  gp::FileDescriptorProto file;
  file.set_name("foxglove/SceneUpdate.proto");
  file.set_package("foxglove");
  file.set_syntax("proto3");
  auto* entity = file.add_message_type();
  entity->set_name("SceneEntity");
  addProtoField(entity, "id", 1, gp::FieldDescriptorProto::TYPE_STRING);
  addProtoField(entity, "timestamp", 2, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(entity, "frame_id", 10, gp::FieldDescriptorProto::TYPE_STRING);  // moved well past official 2
  auto* update = file.add_message_type();
  update->set_name("SceneUpdate");
  addProtoField(update, "deletions", 1, gp::FieldDescriptorProto::TYPE_BYTES, nullptr, true);
  addProtoField(update, "entities", 2, gp::FieldDescriptorProto::TYPE_MESSAGE, ".foxglove.SceneEntity", true);
  gp::FileDescriptorSet fds;
  *fds.add_file() = file;
  std::string out;
  fds.SerializeToString(&out);
  return out;
}

}  // namespace

TEST(ProtobufParserTest, FoxglovePointCloudHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.PointCloud", buildPointCloudVariantSchema()));

  PW pef;
  pef.fixed32(1, 0);                               // offset (variant number)
  pef.varint(2, 7);                                // type = FLOAT32 (variant number)
  pef.str(3, "x");                                 // name (variant number)
  const std::vector<uint8_t> blob = {0, 0, 0, 0};  // one float32 point
  PW pc;
  pc.fixed32(3, 4);        // point_stride (variant number)
  pc.sub(4, pef);          // fields (variant number)
  pc.bytesField(5, blob);  // data (variant number)
  pc.str(6, "lidar");      // frame_id (variant number)
  const auto& wire = pc.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* cloud = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(cloud->frame_id, "lidar");
  EXPECT_EQ(cloud->point_step, 4u);
  EXPECT_EQ(cloud->width, 1u);
  ASSERT_EQ(cloud->fields.size(), 1u);
  EXPECT_EQ(cloud->fields[0].name, "x");
  EXPECT_EQ(cloud->fields[0].offset, 0u);
  EXPECT_EQ(cloud->fields[0].datatype, PJ::sdk::PointField::Datatype::kFloat32);
}

TEST(ProtobufParserTest, FoxgloveSceneUpdateHonorsVariantNestedEntityFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.SceneUpdate", buildSceneUpdateVariantSchema()));

  PW entity;
  entity.str(1, "robot");  // id at field 1
  entity.str(10, "map");   // frame_id at the renumbered field 10
  PW update;
  update.sub(2, entity);  // entities at field 2
  const auto& wire = update.b;

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto rec = base->parseObject(1234, {PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor});
  ASSERT_TRUE(rec.has_value()) << rec.error();
  const auto* scene = std::any_cast<PJ::sdk::SceneEntities>(&rec->object);
  ASSERT_NE(scene, nullptr);
  ASSERT_EQ(scene->entities.size(), 1u);
  EXPECT_EQ(scene->entities[0].id, "robot");
  EXPECT_EQ(scene->entities[0].frame_id, "map");  // nested frame_id resolved from descriptor
}

TEST(ProtobufParserTest, FoxgloveCameraCalibrationDecodes) {
  PW cc;
  cc.sub(1, foxgloveTimestamp(0, 0));
  cc.fixed32(2, 640);  // width
  cc.fixed32(3, 480);  // height
  cc.str(4, "plumb_bob");
  cc.packedDoubles(5, {0.1, 0.2, 0.3, 0.4, 0.5});        // D
  cc.packedDoubles(6, {1, 0, 320, 0, 1, 240, 0, 0, 1});  // K (3x3)
  cc.str(9, "camera_optical");                           // frame_id
  const auto& w = cc.b;

  auto r = pj_protobuf::deserializeFoxgloveCameraCalibration(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  EXPECT_EQ(r->width, 640u);
  EXPECT_EQ(r->height, 480u);
  EXPECT_EQ(r->distortion_model, "plumb_bob");
  EXPECT_EQ(r->frame_id, "camera_optical");
  ASSERT_GE(r->D.size(), 5u);
  EXPECT_DOUBLE_EQ(r->D[0], 0.1);
  EXPECT_DOUBLE_EQ(r->K[0], 1.0);
  EXPECT_DOUBLE_EQ(r->K[2], 320.0);
}

TEST(ProtobufParserTest, FoxgloveFrameTransformDecodes) {
  PW vec3;
  vec3.dbl(1, 1.0);
  vec3.dbl(2, 2.0);
  vec3.dbl(3, 3.0);
  PW quat;
  quat.dbl(1, 0.0);
  quat.dbl(2, 0.0);
  quat.dbl(3, 0.0);
  quat.dbl(4, 1.0);
  PW tf;
  tf.sub(1, foxgloveTimestamp(5, 0));
  tf.str(2, "world");
  tf.str(3, "base_link");
  tf.sub(4, vec3);
  tf.sub(5, quat);
  const auto& w = tf.b;

  auto r = pj_protobuf::deserializeFoxgloveFrameTransform(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->transforms.size(), 1u);
  const auto& t = r->transforms[0];
  EXPECT_EQ(t.parent_frame_id, "world");
  EXPECT_EQ(t.child_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(t.translation.x, 1.0);
  EXPECT_DOUBLE_EQ(t.translation.z, 3.0);
  EXPECT_DOUBLE_EQ(t.rotation.w, 1.0);
  EXPECT_EQ(t.timestamp, 5'000'000'000LL);
}

TEST(ProtobufParserTest, FoxgloveImageAnnotationsDecodes) {
  // One PointsAnnotation (field 2), type POINTS, with two Point2 vertices.
  PW p0;
  p0.dbl(1, 10.0);
  p0.dbl(2, 20.0);
  PW p1;
  p1.dbl(1, 30.0);
  p1.dbl(2, 40.0);
  PW pa;
  pa.varint(2, 1);  // type = POINTS (1)
  pa.sub(3, p0);    // points[0]
  pa.sub(3, p1);    // points[1]
  PW ann;
  ann.sub(2, pa);  // points (PointsAnnotation)
  const auto& w = ann.b;

  auto r = pj_protobuf::deserializeFoxgloveImageAnnotations(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->points.size(), 1u);
  const auto& pts = r->points[0];
  EXPECT_EQ(pts.topology, PJ::sdk::AnnotationTopology::kPoints);
  ASSERT_EQ(pts.points.size(), 2u);
  EXPECT_DOUBLE_EQ(pts.points[0].x, 10.0);
  EXPECT_DOUBLE_EQ(pts.points[1].y, 40.0);
}

TEST(ProtobufParserTest, FoxgloveSceneUpdateDecodes) {
  PW entity;
  entity.sub(1, foxgloveTimestamp(3, 0));  // timestamp
  entity.str(2, "map");                    // frame_id
  entity.str(3, "robot");                  // id
  PW scene;
  scene.sub(2, entity);  // entities
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  EXPECT_EQ(r->entities[0].frame_id, "map");
  EXPECT_EQ(r->entities[0].id, "robot");
  EXPECT_EQ(r->entities[0].timestamp, 3'000'000'000LL);
}

// Exercises readColor/toU8 (the [0,1]→uint8 rounding) and the readBoxLike /
// readCylinder primitive readers — none of which had any coverage. Color
// {1.0, 0.5, 0.0, 1.0} must round to {255, 128, 0, 255} (lround, not truncation:
// 0.5*255 = 127.5 → 128).
TEST(ProtobufParserTest, FoxgloveSceneUpdateDecodesCubeColorAndCylinderScales) {
  PW color;  // foxglove Color: double r/g/b/a in [0,1]
  color.dbl(1, 1.0);
  color.dbl(2, 0.5);
  color.dbl(3, 0.0);
  color.dbl(4, 1.0);

  PW cube_size;  // Vector3 (2, 3, 4)
  cube_size.dbl(1, 2.0);
  cube_size.dbl(2, 3.0);
  cube_size.dbl(3, 4.0);
  PW cube;  // CubePrimitive { pose=1, size=2, color=3 } — pose omitted → identity
  cube.sub(2, cube_size);
  cube.sub(3, color);

  PW cyl_size;
  cyl_size.dbl(1, 1.0);
  cyl_size.dbl(2, 1.0);
  cyl_size.dbl(3, 2.0);
  PW cylinder;  // CylinderPrimitive { pose=1, size=2, bottom_scale=3, top_scale=4, color=5 }
  cylinder.sub(2, cyl_size);
  cylinder.dbl(3, 0.5);  // bottom_scale
  cylinder.dbl(4, 0.8);  // top_scale
  cylinder.sub(5, color);

  PW entity;
  entity.str(3, "shapes");
  entity.sub(8, cube);       // cubes[0]
  entity.sub(10, cylinder);  // cylinders[0]
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  const auto& e = r->entities[0];
  ASSERT_EQ(e.cubes.size(), 1u);
  EXPECT_DOUBLE_EQ(e.cubes[0].size.x, 2.0);
  EXPECT_DOUBLE_EQ(e.cubes[0].size.z, 4.0);
  EXPECT_EQ(e.cubes[0].color, (PJ::sdk::ColorRGBA{255, 128, 0, 255}));
  ASSERT_EQ(e.cylinders.size(), 1u);
  EXPECT_DOUBLE_EQ(e.cylinders[0].bottom_scale, 0.5);
  EXPECT_DOUBLE_EQ(e.cylinders[0].top_scale, 0.8);
  EXPECT_EQ(e.cylinders[0].color, (PJ::sdk::ColorRGBA{255, 128, 0, 255}));
}

// Characterizes the lenient-reader contract the field-scanner sugar must keep:
// a field with the RIGHT number but the WRONG wire type is SKIPPED and decoding
// CONTINUES with the next field (it is not fatal). Here a cube's Vector3 size
// encodes x (field 1) as a varint instead of a double; the decoder must drop x
// (stays 0) yet still read y (field 2). A refactor that treated a wrong-wire
// field as a hard stop would lose y too.
TEST(ProtobufParserTest, FoxgloveSceneUpdateSkipsWrongWireTypeFieldAndContinues) {
  PW cube_size;             // Vector3 with a deliberately malformed x
  cube_size.varint(1, 42);  // x: wire type varint (should be I64 double) -> skipped
  cube_size.dbl(2, 3.0);    // y: well-formed, must still decode
  cube_size.dbl(3, 4.0);    // z: well-formed
  PW cube;
  cube.sub(2, cube_size);  // CubePrimitive { size=2 }
  PW entity;
  entity.str(3, "lenient");
  entity.sub(8, cube);
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  ASSERT_EQ(r->entities[0].cubes.size(), 1u);
  const auto& size = r->entities[0].cubes[0].size;
  EXPECT_DOUBLE_EQ(size.x, 0.0);  // malformed field dropped
  EXPECT_DOUBLE_EQ(size.y, 3.0);  // decoding continued past it
  EXPECT_DOUBLE_EQ(size.z, 4.0);
}

// Companion to the above at the SceneEntity level: an unknown/handled-elsewhere
// field (here field 6, metadata, which the decoder intentionally skips) sitting
// between two real primitives must not disturb either. Pins the catch-all skip
// of the entity-level field loop.
TEST(ProtobufParserTest, FoxgloveSceneUpdateSkipsUnhandledEntityFieldBetweenPrimitives) {
  PW cube_size;
  cube_size.dbl(1, 1.0);
  cube_size.dbl(2, 1.0);
  cube_size.dbl(3, 1.0);
  PW cube;
  cube.sub(2, cube_size);

  PW metadata;  // KeyValuePair-ish blob; the decoder skips field 6 wholesale
  metadata.str(1, "k");
  metadata.str(2, "v");

  PW sphere_size;
  sphere_size.dbl(1, 0.5);
  sphere_size.dbl(2, 0.5);
  sphere_size.dbl(3, 0.5);
  PW sphere;
  sphere.sub(2, sphere_size);

  PW entity;
  entity.str(3, "interleaved");
  entity.sub(8, cube);      // cubes[0]
  entity.sub(6, metadata);  // skipped
  entity.sub(9, sphere);    // spheres[0] — must survive the skip
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  EXPECT_EQ(r->entities[0].cubes.size(), 1u);
  ASSERT_EQ(r->entities[0].spheres.size(), 1u);
  EXPECT_DOUBLE_EQ(r->entities[0].spheres[0].size.x, 0.5);
}

// SceneEntity.models (field 14) carries mesh assets. Pre-fix readSceneEntity let
// field 14 fall through to skipField, so an inline glTF model was silently
// dropped and never rendered. An inline model provides `data` tagged by
// `media_type`; `url` stays empty (mirrors the real Waymo /marker/car entity).
TEST(ProtobufParserTest, FoxgloveSceneUpdateDecodesInlineModel) {
  PW scale;  // Vector3 (2.14, 2.1, 2.1)
  scale.dbl(1, 2.14);
  scale.dbl(2, 2.1);
  scale.dbl(3, 2.1);
  const std::vector<uint8_t> glb = {'g', 'l', 'T', 'F', 0x02, 0x00, 0xAB, 0xCD, 0xEF};
  PW model;  // ModelPrimitive { scale=2, media_type=6, data=7 }
  model.sub(2, scale);
  model.str(6, "model/gltf-binary");
  model.bytesField(7, glb);
  PW entity;
  entity.str(2, "base_link");  // frame_id
  entity.str(3, "waymo_car");  // id
  entity.sub(14, model);       // models[0]
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  const auto& e = r->entities[0];
  ASSERT_EQ(e.models.size(), 1u);
  EXPECT_EQ(e.models[0].media_type, "model/gltf-binary");
  EXPECT_EQ(e.models[0].data, glb);  // full bytes, not truncated
  EXPECT_DOUBLE_EQ(e.models[0].scale.x, 2.14);
  EXPECT_TRUE(e.models[0].url.empty());
}

// A model can instead reference an external resource via `url`, leaving `data`
// empty. Confirms both source branches of ModelPrimitive decode.
TEST(ProtobufParserTest, FoxgloveSceneUpdateDecodesUrlModel) {
  PW model;  // ModelPrimitive { url=5 }
  model.str(5, "package://meshes/car.glb");
  PW entity;
  entity.sub(14, model);
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  ASSERT_EQ(r->entities[0].models.size(), 1u);
  EXPECT_EQ(r->entities[0].models[0].url, "package://meshes/car.glb");
  EXPECT_TRUE(r->entities[0].models[0].data.empty());
}

// Regression for the field-14 fall-through: an entity carrying BOTH a cube
// (field 8) and a model (field 14) must emit both. Proves wiring models does
// not steal/skip the geometry and vice versa.
TEST(ProtobufParserTest, FoxgloveSceneUpdateDecodesCubeAndModelTogether) {
  PW cube_size;  // Vector3
  cube_size.dbl(1, 1.0);
  cube_size.dbl(2, 1.0);
  cube_size.dbl(3, 1.0);
  PW cube;  // CubePrimitive { size=2 }
  cube.sub(2, cube_size);

  const std::vector<uint8_t> glb = {'g', 'l', 'T', 'F', 0x01};
  PW model;  // ModelPrimitive { media_type=6, data=7 }
  model.str(6, "model/gltf-binary");
  model.bytesField(7, glb);

  PW entity;
  entity.str(3, "mixed");
  entity.sub(8, cube);    // cubes[0]
  entity.sub(14, model);  // models[0]
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  const auto& e = r->entities[0];
  EXPECT_EQ(e.cubes.size(), 1u);
  EXPECT_EQ(e.models.size(), 1u);
  EXPECT_EQ(e.models[0].data, glb);
}

// A corrupt `data` length must not be trusted: the varint declares 1000 bytes
// but only 5 exist in the submessage. Pre-fix, readBytes resized to the
// declared length BEFORE checking availability, so the model came back with a
// 1000-byte zero-filled buffer presented as successfully decoded (and a
// declared 4 GiB length would have allocated 4 GiB on a few bytes of input).
// The length must be validated against the bytes remaining; on mismatch the
// model decodes with empty data.
TEST(ProtobufParserTest, FoxgloveSceneUpdateRejectsOverdeclaredModelData) {
  PW model;  // ModelPrimitive { media_type=6, data=7 (malformed) }
  model.str(6, "model/gltf-binary");
  model.tag(7, 2);        // data: length-delimited...
  model.rawVarint(1000);  // ...declaring 1000 payload bytes...
  const std::vector<uint8_t> partial = {'g', 'l', 'T', 'F', 0x02};
  model.b.insert(model.b.end(), partial.begin(), partial.end());  // ...with only 5 present
  PW entity;
  entity.str(3, "corrupt");
  entity.sub(14, model);  // submessage length is honest; the lie is inside
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  ASSERT_EQ(r->entities[0].models.size(), 1u);
  EXPECT_TRUE(r->entities[0].models[0].data.empty());  // not a zero-filled 1000-byte buffer
}

// proto3 omits a 0.0 scale; foxglove reads an omitted scale as 0 (collapsed
// face), so the decoder must override the SDK's ergonomic 1.0 default. Without
// the fix these decode to 1.0.
TEST(ProtobufParserTest, FoxgloveCylinderOmittedScalesDecodeToZero) {
  PW cyl_size;
  cyl_size.dbl(1, 1.0);
  cyl_size.dbl(2, 1.0);
  cyl_size.dbl(3, 2.0);
  PW cylinder;  // size only — bottom_scale (3) / top_scale (4) omitted
  cylinder.sub(2, cyl_size);
  PW entity;
  entity.sub(10, cylinder);
  PW scene;
  scene.sub(2, entity);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 1u);
  ASSERT_EQ(r->entities[0].cylinders.size(), 1u);
  EXPECT_DOUBLE_EQ(r->entities[0].cylinders[0].bottom_scale, 0.0);
  EXPECT_DOUBLE_EQ(r->entities[0].cylinders[0].top_scale, 0.0);
}

// A 180° rotation about X is {x=1, y=0, z=0, w=0}; proto3 omits the zero fields,
// leaving only x on the wire. The decoder must NOT fall back to the SDK's
// identity default (w=1) — that would silently corrupt the rotation.
TEST(ProtobufParserTest, FoxgloveQuaternionOmittedWDecodesToZero) {
  PW quat;
  quat.dbl(1, 1.0);  // x = 1; y, z, w omitted (proto3 zero defaults)
  PW tf;
  tf.str(2, "world");
  tf.str(3, "child");
  tf.sub(5, quat);  // rotation
  const auto& w = tf.b;

  auto r = pj_protobuf::deserializeFoxgloveFrameTransform(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->transforms.size(), 1u);
  const auto& rot = r->transforms[0].rotation;
  EXPECT_DOUBLE_EQ(rot.x, 1.0);
  EXPECT_DOUBLE_EQ(rot.y, 0.0);
  EXPECT_DOUBLE_EQ(rot.z, 0.0);
  EXPECT_DOUBLE_EQ(rot.w, 0.0);  // NOT 1.0
}

// foxglove.ImageAnnotations has no top-level timestamp; the decoder must adopt
// the first sub-annotation's stamp so the overlay can time-align to its image.
TEST(ProtobufParserTest, FoxgloveImageAnnotationsAdoptsTimestamp) {
  PW p0;
  p0.dbl(1, 10.0);
  p0.dbl(2, 20.0);
  PW pa;
  pa.sub(1, foxgloveTimestamp(8, 500));  // PointsAnnotation.timestamp
  pa.varint(2, 1);                       // type = POINTS
  pa.sub(3, p0);
  PW ann;
  ann.sub(2, pa);
  const auto& w = ann.b;

  auto r = pj_protobuf::deserializeFoxgloveImageAnnotations(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  EXPECT_EQ(r->timestamp, 8'000'000'500LL);
  ASSERT_EQ(r->points.size(), 1u);
  ASSERT_EQ(r->points[0].points.size(), 1u);
}

// Regression for the SubMessage desync: entity 1 ends with an unhandled wire
// type (group-start, wire 3) that makes readSceneEntity break mid-submessage,
// followed by trailing bytes. The SubMessage dtor must skip to the entity
// boundary so entity 2 still decodes cleanly. Pre-fix, PopLimit alone left the
// parent stranded mid-entity-1 and every following sibling misparsed.
TEST(ProtobufParserTest, FoxgloveSceneUpdateRecoversFromMalformedEntityField) {
  PW e1;
  e1.str(2, "frameA");
  e1.str(3, "id1");
  e1.tag(20, 3);         // unhandled wire type → break inside readSceneEntity
  e1.b.push_back(0x55);  // trailing bytes the broken reader leaves unconsumed
  e1.b.push_back(0x66);
  PW e2;
  e2.str(2, "map");
  e2.str(3, "robot");
  PW scene;
  scene.sub(2, e1);
  scene.sub(2, e2);
  const auto& w = scene.b;

  auto r = pj_protobuf::deserializeFoxgloveSceneUpdate(w.data(), w.size());
  ASSERT_TRUE(r) << r.error();
  ASSERT_EQ(r->entities.size(), 2u);
  EXPECT_EQ(r->entities[0].frame_id, "frameA");
  EXPECT_EQ(r->entities[0].id, "id1");
  EXPECT_EQ(r->entities[1].frame_id, "map");
  EXPECT_EQ(r->entities[1].id, "robot");
}

// ---------------------------------------------------------------------------
// foxglove.LaserScan → kPointCloud (eager projection via pj_laser_scan)
//
// Wire layout (verified against foxglove-sdk schemas/proto/foxglove/LaserScan.proto):
//   timestamp = 1 (google.protobuf.Timestamp), frame_id = 2 (string),
//   pose = 3 (foxglove.Pose), start_angle = 4 (double), end_angle = 5 (double),
//   ranges = 6 (repeated double, packed), intensities = 7 (repeated double, packed).
// ---------------------------------------------------------------------------

namespace {

constexpr double kDNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kDInf = std::numeric_limits<double>::infinity();

std::vector<uint8_t> buildFoxgloveLaserScanWire(
    int64_t ts_sec, int32_t ts_nanos, const std::string& frame_id, double start_angle, double end_angle,
    const std::vector<double>& ranges, const std::vector<double>& intensities, bool with_pose, double pose_tx) {
  PW scan;
  scan.sub(1, foxgloveTimestamp(ts_sec, ts_nanos));
  scan.str(2, frame_id);
  if (with_pose) {
    PW pos;
    pos.dbl(1, pose_tx);
    PW quat;
    quat.dbl(4, 1.0);  // identity orientation, explicit w
    PW pose;
    pose.sub(1, pos);
    pose.sub(2, quat);
    scan.sub(3, pose);
  }
  scan.dbl(4, start_angle);
  scan.dbl(5, end_angle);
  if (!ranges.empty()) {
    scan.packedDoubles(6, ranges);
  }
  if (!intensities.empty()) {
    scan.packedDoubles(7, intensities);
  }
  return scan.b;
}

/// Reads the float32 at byte offset `off` of point `index` in a packed cloud.
float scanCloudFloat(const PJ::sdk::PointCloud& cloud, uint32_t index, uint32_t off) {
  float v = 0.0f;
  std::memcpy(&v, cloud.data.data() + static_cast<size_t>(index) * cloud.point_step + off, sizeof(float));
  return v;
}

}  // namespace

TEST(ProtobufParserTest, FoxgloveLaserScanCodecProjectsAndCachesLut) {
  // 5 rays from -1.0 to 1.0 rad → angle_increment = 0.5; NaN/Inf drop
  // (foxglove carries no range bounds, so only non-finite rays drop).
  const std::vector<double> ranges = {1.0, kDNaN, 2.0, kDInf, 3.0};
  const auto wire = buildFoxgloveLaserScanWire(7, 42, "lidar_2d", -1.0, 1.0, ranges, {}, false, 0.0);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const auto& cloud = decoded->cloud;

  EXPECT_EQ(cloud.frame_id, "lidar_2d");
  EXPECT_EQ(cloud.timestamp_ns, 7'000'000'042LL);
  EXPECT_EQ(cloud.height, 1u);
  EXPECT_EQ(cloud.width, 3u);
  EXPECT_EQ(cloud.point_step, 12u);
  EXPECT_EQ(cloud.row_step, 3u * 12u);
  EXPECT_TRUE(cloud.is_dense);
  EXPECT_FALSE(cloud.is_bigendian);
  ASSERT_EQ(cloud.fields.size(), 3u);
  EXPECT_EQ(cloud.fields[0].name, "x");
  EXPECT_EQ(cloud.fields[1].name, "y");
  EXPECT_EQ(cloud.fields[2].name, "z");
  EXPECT_EQ(decoded->ray_count, 5u);
  EXPECT_DOUBLE_EQ(decoded->start_angle, -1.0);
  EXPECT_DOUBLE_EQ(decoded->end_angle, 1.0);
  EXPECT_FALSE(decoded->has_pose);

  // Kept rays 0/2/4 at theta = -1.0 / 0.0 / 1.0.
  EXPECT_EQ(scanCloudFloat(cloud, 0, 0), 1.0f * static_cast<float>(std::cos(-1.0)));
  EXPECT_EQ(scanCloudFloat(cloud, 0, 4), 1.0f * static_cast<float>(std::sin(-1.0)));
  EXPECT_EQ(scanCloudFloat(cloud, 1, 0), 2.0f);
  EXPECT_EQ(scanCloudFloat(cloud, 1, 4), 0.0f);
  EXPECT_EQ(scanCloudFloat(cloud, 2, 0), 3.0f * static_cast<float>(std::cos(1.0)));

  // The owned buffer is anchored — the cloud must not alias the wire bytes.
  ASSERT_NE(cloud.anchor, nullptr);
  const auto* lo = cloud.data.data();
  const auto* hi = cloud.data.data() + cloud.data.size();
  EXPECT_TRUE(hi <= wire.data() || lo >= wire.data() + wire.size());

  // Same scan config decoded again → the cos/sin LUT is reused, not rebuilt.
  EXPECT_EQ(projector.lutRebuildCount(), 1u);
  auto decoded2 = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded2.has_value());
  EXPECT_EQ(projector.lutRebuildCount(), 1u);
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecIntensityPassthrough) {
  const std::vector<double> ranges = {1.0, kDNaN, 3.0};
  const std::vector<double> intensities = {10.0, 20.0, 30.0};
  const auto wire = buildFoxgloveLaserScanWire(0, 0, "lidar", 0.0, 0.2, ranges, intensities, false, 0.0);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const auto& cloud = decoded->cloud;

  ASSERT_EQ(cloud.width, 2u);
  EXPECT_EQ(cloud.point_step, 16u);
  ASSERT_EQ(cloud.fields.size(), 4u);
  EXPECT_EQ(cloud.fields[3].name, "intensity");
  EXPECT_EQ(scanCloudFloat(cloud, 0, 12), 10.0f);
  EXPECT_EQ(scanCloudFloat(cloud, 1, 12), 30.0f);  // follows its ray through the drop
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecIntensitySizeMismatchIgnored) {
  const std::vector<double> ranges = {1.0, 2.0, 3.0};
  const std::vector<double> intensities = {10.0};  // wrong size → xyz-only
  const auto wire = buildFoxgloveLaserScanWire(0, 0, "lidar", 0.0, 0.2, ranges, intensities, false, 0.0);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded->cloud.point_step, 12u);
  EXPECT_EQ(decoded->cloud.fields.size(), 3u);
  EXPECT_EQ(decoded->cloud.width, 3u);
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecSingleRay) {
  // N = 1: the (end-start)/(N-1) increment formula must not divide by zero.
  const auto wire = buildFoxgloveLaserScanWire(0, 0, "lidar", 0.5, 0.5, {2.0}, {}, false, 0.0);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  ASSERT_EQ(decoded->cloud.width, 1u);
  EXPECT_EQ(scanCloudFloat(decoded->cloud, 0, 0), 2.0f * static_cast<float>(std::cos(0.5)));
  EXPECT_EQ(scanCloudFloat(decoded->cloud, 0, 4), 2.0f * static_cast<float>(std::sin(0.5)));
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecEmptyScan) {
  const auto wire = buildFoxgloveLaserScanWire(1, 0, "lidar", 0.0, 0.0, {}, {}, false, 0.0);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded->cloud.width, 0u);
  EXPECT_EQ(decoded->cloud.data.size(), 0u);
  EXPECT_EQ(decoded->ray_count, 0u);
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecAcceptsUnpackedRanges) {
  // Spec-compliant parsers must accept repeated doubles in unpacked encoding.
  PW scan;
  scan.str(2, "lidar");
  scan.dbl(4, 0.0);
  scan.dbl(5, 0.1);
  scan.dbl(6, 1.0);  // ranges as individual I64 entries
  scan.dbl(6, 2.0);
  const auto& wire = scan.b;

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_EQ(decoded->ray_count, 2u);
  EXPECT_EQ(decoded->cloud.width, 2u);
}

TEST(ProtobufParserTest, FoxgloveLaserScanCodecFlagsNonIdentityPose) {
  const auto wire = buildFoxgloveLaserScanWire(0, 0, "lidar", 0.0, 0.1, {1.0, 2.0}, {}, true, 3.5);

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_TRUE(decoded->has_pose);
  EXPECT_FALSE(decoded->pose_is_identity);  // translation x = 3.5 → not identity
}

TEST(ProtobufParserTest, FoxgloveLaserScanObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  // Empty schema bytes: the canonical fast path keys off the type name only.
  ASSERT_TRUE(f.bindSchema("foxglove.LaserScan", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.LaserScan", empty_schema), PJ::sdk::BuiltinObjectType::kPointCloud);

  const auto wire = buildFoxgloveLaserScanWire(7, 0, "lidar_2d", -0.5, 0.5, {1.0, kDNaN, 2.0}, {}, false, 0.0);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_FALSE(rec->ts.has_value());  // embedded ts disabled by default

  const auto* pc = std::any_cast<PJ::sdk::PointCloud>(&rec->object);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->frame_id, "lidar_2d");
  EXPECT_EQ(pc->width, 2u);
  EXPECT_EQ(pc->point_step, 12u);
  EXPECT_EQ(pc->timestamp_ns, 7'000'000'000LL);
  ASSERT_NE(pc->anchor, nullptr);
}

TEST(ProtobufParserTest, FoxgloveLaserScanScalarRoute) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.LaserScan", std::string{}));

  const auto wire = buildFoxgloveLaserScanWire(7, 250, "lidar_2d", -0.5, 0.5, {1.0, kDNaN, 2.0}, {}, false, 0.0);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

  const auto* timestamp = PJ::sdk::testing::ParserWriteRecorder::findField(row, "timestamp");
  ASSERT_NE(timestamp, nullptr);
  EXPECT_DOUBLE_EQ(timestamp->numeric, static_cast<double>(7'000'000'250LL) * 1e-9);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "lidar_2d");

  const auto* start_angle = PJ::sdk::testing::ParserWriteRecorder::findField(row, "start_angle");
  ASSERT_NE(start_angle, nullptr);
  EXPECT_DOUBLE_EQ(start_angle->numeric, -0.5);

  const auto* end_angle = PJ::sdk::testing::ParserWriteRecorder::findField(row, "end_angle");
  ASSERT_NE(end_angle, nullptr);
  EXPECT_DOUBLE_EQ(end_angle->numeric, 0.5);

  const auto* num_ranges = PJ::sdk::testing::ParserWriteRecorder::findField(row, "num_ranges");
  ASSERT_NE(num_ranges, nullptr);
  EXPECT_DOUBLE_EQ(num_ranges->numeric, 3.0);

  // point_count is NOT emitted: the scalar route is a header-only walk and
  // never projects, so the kept-point count is unknowable here.
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(row, "point_count"), nullptr);
}

// Regression: a packed-ranges LEN varint claiming 1 GiB with the buffer ending
// right after must fail cleanly — and must never drive a giant up-front
// reserve (the capped reserve guarantees this regardless of how the
// CodedInputStream is constructed; the flat-array constructor's implicit
// buffer-size limit was previously the only thing preventing it).
TEST(ProtobufParserTest, FoxgloveLaserScanCorruptRangesLengthFailsCleanly) {
  PW scan;
  scan.str(2, "lidar");
  scan.dbl(4, 0.0);
  scan.dbl(5, 0.1);
  scan.tag(6, 2);                     // ranges, LEN wire type…
  scan.rawVarint(uint64_t{1} << 30);  // …claiming 1 GiB of doubles,
  const auto& wire = scan.b;          // but the buffer ends here.

  PJ::laser_scan::LaserScanProjector projector;
  auto decoded = pj_protobuf::deserializeFoxgloveLaserScan(wire.data(), wire.size(), projector);
  EXPECT_FALSE(decoded.has_value());  // object route: clean error, no crash

  auto info = pj_protobuf::readFoxgloveLaserScanInfo(wire.data(), wire.size());
  EXPECT_FALSE(info.has_value());  // scalar-route walk rejects it too
}

// ---------------------------------------------------------------------------
// foxglove.PosesInFrame / PJ.PosesInFrame -> kPosesInFrame
//
// foxglove.PosesInFrame is wire-identical to the canonical PJ.PosesInFrame
// (the SDK proto mirrors it field-for-field), so the parser binds both names
// to the same SDK codec — exactly the VideoFrame precedent. We build a genuine
// payload with the real protobuf serializer and exercise the parser's object
// route. Wire layout:
//   PosesInFrame { timestamp=1 (Timestamp), frame_id=2 (string),
//                  poses=3 (repeated Pose) }
//   Pose { position=1 (Vector3 x/y/z), orientation=2 (Quaternion x/y/z/w) }
// ---------------------------------------------------------------------------

namespace {

struct PoseValues {
  double px, py, pz;      // position
  double qx, qy, qz, qw;  // orientation
};

std::vector<uint8_t> buildPosesInFrameWire(
    int64_t ts_sec, int32_t ts_nanos, const std::string& frame_id, const std::vector<PoseValues>& poses) {
  gp::FileDescriptorProto file;
  file.set_name("pif.proto");
  file.set_syntax("proto3");
  file.set_package("test");

  auto add_field = [](gp::DescriptorProto* m, const char* name, int num, gp::FieldDescriptorProto::Type type,
                      const char* type_name = nullptr, bool repeated = false) {
    auto* f = m->add_field();
    f->set_name(name);
    f->set_number(num);
    f->set_type(type);
    f->set_label(repeated ? gp::FieldDescriptorProto::LABEL_REPEATED : gp::FieldDescriptorProto::LABEL_OPTIONAL);
    if (type_name != nullptr) {
      f->set_type_name(type_name);
    }
  };

  auto* ts = file.add_message_type();
  ts->set_name("Timestamp");
  add_field(ts, "seconds", 1, gp::FieldDescriptorProto::TYPE_INT64);
  add_field(ts, "nanos", 2, gp::FieldDescriptorProto::TYPE_INT32);

  auto* vec = file.add_message_type();
  vec->set_name("Vector3");
  add_field(vec, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(vec, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* quat = file.add_message_type();
  quat->set_name("Quaternion");
  add_field(quat, "x", 1, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "y", 2, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "z", 3, gp::FieldDescriptorProto::TYPE_DOUBLE);
  add_field(quat, "w", 4, gp::FieldDescriptorProto::TYPE_DOUBLE);

  auto* pose = file.add_message_type();
  pose->set_name("Pose");
  add_field(pose, "position", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Vector3");
  add_field(pose, "orientation", 2, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Quaternion");

  auto* pif = file.add_message_type();
  pif->set_name("PosesInFrame");
  add_field(pif, "timestamp", 1, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Timestamp");
  add_field(pif, "frame_id", 2, gp::FieldDescriptorProto::TYPE_STRING);
  add_field(pif, "poses", 3, gp::FieldDescriptorProto::TYPE_MESSAGE, ".test.Pose", true);

  gp::DescriptorPool pool;
  const gp::FileDescriptor* fd = pool.BuildFile(file);
  const gp::Descriptor* pif_desc = fd->FindMessageTypeByName("PosesInFrame");
  gp::DynamicMessageFactory factory;
  std::unique_ptr<gp::Message> msg(factory.GetPrototype(pif_desc)->New());
  const gp::Reflection* ref = msg->GetReflection();

  gp::Message* tsm = ref->MutableMessage(msg.get(), pif_desc->FindFieldByName("timestamp"), &factory);
  const gp::Descriptor* tsd = tsm->GetDescriptor();
  tsm->GetReflection()->SetInt64(tsm, tsd->FindFieldByName("seconds"), ts_sec);
  tsm->GetReflection()->SetInt32(tsm, tsd->FindFieldByName("nanos"), ts_nanos);

  ref->SetString(msg.get(), pif_desc->FindFieldByName("frame_id"), frame_id);

  const gp::FieldDescriptor* poses_f = pif_desc->FindFieldByName("poses");
  for (const auto& pv : poses) {
    gp::Message* pm = ref->AddMessage(msg.get(), poses_f, &factory);
    const gp::Descriptor* pd = pm->GetDescriptor();
    gp::Message* pos = pm->GetReflection()->MutableMessage(pm, pd->FindFieldByName("position"), &factory);
    const gp::Descriptor* vd = pos->GetDescriptor();
    pos->GetReflection()->SetDouble(pos, vd->FindFieldByName("x"), pv.px);
    pos->GetReflection()->SetDouble(pos, vd->FindFieldByName("y"), pv.py);
    pos->GetReflection()->SetDouble(pos, vd->FindFieldByName("z"), pv.pz);
    gp::Message* ori = pm->GetReflection()->MutableMessage(pm, pd->FindFieldByName("orientation"), &factory);
    const gp::Descriptor* qd = ori->GetDescriptor();
    ori->GetReflection()->SetDouble(ori, qd->FindFieldByName("x"), pv.qx);
    ori->GetReflection()->SetDouble(ori, qd->FindFieldByName("y"), pv.qy);
    ori->GetReflection()->SetDouble(ori, qd->FindFieldByName("z"), pv.qz);
    ori->GetReflection()->SetDouble(ori, qd->FindFieldByName("w"), pv.qw);
  }

  std::string out;
  msg->SerializeToString(&out);
  return std::vector<uint8_t>(out.begin(), out.end());
}

void expectTwoPosesDecoded(const PJ::sdk::PosesInFrame& pf, double half_sqrt2) {
  ASSERT_EQ(pf.poses.size(), 2u);
  EXPECT_DOUBLE_EQ(pf.poses[0].position.x, 1.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].position.y, 2.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].position.z, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].orientation.x, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].orientation.y, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].orientation.z, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[0].orientation.w, 1.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].position.x, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].position.y, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].position.z, 1.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].orientation.x, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].orientation.y, 0.0);
  EXPECT_DOUBLE_EQ(pf.poses[1].orientation.z, half_sqrt2);
  EXPECT_DOUBLE_EQ(pf.poses[1].orientation.w, half_sqrt2);
}

const std::vector<PoseValues>& twoSamplePoses() {
  static const double half_sqrt2 = 0.7071067811865476;  // sin/cos(45°): a 90° yaw quaternion
  static const std::vector<PoseValues> poses = {
      {.px = 1.0, .py = 2.0, .pz = 0.0, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0},
      {.px = 0.0, .py = 0.0, .pz = 1.0, .qx = 0.0, .qy = 0.0, .qz = half_sqrt2, .qw = half_sqrt2},
  };
  return poses;
}

}  // namespace

TEST(ProtobufParserTest, FoxglovePosesInFrameObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  // Empty schema bytes: the well-known fast path keys off the type name only.
  ASSERT_TRUE(f.bindSchema("foxglove.PosesInFrame", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.PosesInFrame", empty_schema), PJ::sdk::BuiltinObjectType::kPosesInFrame);

  const auto wire = buildPosesInFrameWire(9, 500, "map", twoSamplePoses());

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pf = std::any_cast<PJ::sdk::PosesInFrame>(&rec->object);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(pf->frame_id, "map");
  EXPECT_EQ(pf->timestamp_ns, 9'000'000'500LL);  // decoded from the wire timestamp
  EXPECT_FALSE(rec->ts.has_value());             // embedded-ts option off by default
  expectTwoPosesDecoded(*pf, 0.7071067811865476);
}

TEST(ProtobufParserTest, FoxglovePosesInFrameScalarRoute) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.PosesInFrame", std::string{}));

  const auto wire = buildPosesInFrameWire(9, 500, "map", twoSamplePoses());
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  // Slim metadata row: a pose array can be huge, so the scalar route emits the
  // header pair plus a bounded count (mirroring foxglove.FrameTransform's
  // num_transforms), never per-pose columns.
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  const auto* num_poses = PJ::sdk::testing::ParserWriteRecorder::findField(row, "num_poses");
  ASSERT_NE(num_poses, nullptr);
  EXPECT_DOUBLE_EQ(num_poses->numeric, 2.0);

  const auto* timestamp = PJ::sdk::testing::ParserWriteRecorder::findField(row, "timestamp");
  ASSERT_NE(timestamp, nullptr);
  EXPECT_DOUBLE_EQ(timestamp->numeric, static_cast<double>(9'000'000'500LL) * 1e-9);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "map");
}

// The canonical PJ.PosesInFrame schema name binds to the same SDK codec as the
// foxglove name (the protos are wire-identical), so a PlotJuggler-native source
// promotes to kPosesInFrame too.
TEST(ProtobufParserTest, PjPosesInFrameObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  ASSERT_TRUE(f.bindSchema("PJ.PosesInFrame", std::string{}));  // == kSchemaPosesInFrame
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("PJ.PosesInFrame", empty_schema), PJ::sdk::BuiltinObjectType::kPosesInFrame);

  const auto wire = buildPosesInFrameWire(9, 500, "map", twoSamplePoses());

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pf = std::any_cast<PJ::sdk::PosesInFrame>(&rec->object);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(pf->frame_id, "map");
  EXPECT_EQ(pf->timestamp_ns, 9'000'000'500LL);
  expectTwoPosesDecoded(*pf, 0.7071067811865476);
}

// ---------------------------------------------------------------------------
// foxglove.PoseInFrame (singular) -> kPosesInFrame (one pose)
//
// PoseInFrame { timestamp=1, frame_id=2, pose=3 (single Pose) } is wire-identical
// to a one-element PosesInFrame (field 3, length-delimited submessage), so the
// parser binds it to the same SDK codec — the protobuf analog of ROS PoseStamped.
// ---------------------------------------------------------------------------

TEST(ProtobufParserTest, FoxglovePoseInFrameObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  ASSERT_TRUE(f.bindSchema("foxglove.PoseInFrame", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.PoseInFrame", empty_schema), PJ::sdk::BuiltinObjectType::kPosesInFrame);

  // One pose: the wire bytes are exactly what a foxglove.PoseInFrame produces.
  const std::vector<PoseValues> one = {{.px = 9.0, .py = 8.0, .pz = 7.0, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0}};
  const auto wire = buildPosesInFrameWire(3, 0, "base_link", one);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pf = std::any_cast<PJ::sdk::PosesInFrame>(&rec->object);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(pf->frame_id, "base_link");
  EXPECT_EQ(pf->timestamp_ns, 3'000'000'000LL);
  EXPECT_FALSE(rec->ts.has_value());  // embedded-ts off by default
  ASSERT_EQ(pf->poses.size(), 1u);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.x, 9.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.y, 8.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.z, 7.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].orientation.w, 1.0);
}

// ---------------------------------------------------------------------------
// foxglove.Odometry -> kPosesInFrame (one pose, the protobuf analog of ROS
// nav_msgs/Odometry).
//
// Odometry { timestamp=1, frame_id=2, body_frame_id=3, pose=4 Pose,
//   linear_velocity=5, angular_velocity=6, pose_covariance=7 (repeated double),
//   velocity_covariance=8, metadata=9 }. Unlike PoseInFrame the pose sits at
//   field 4 (field 3 is a string), so the PosesInFrame codec cannot be reused —
//   a dedicated decoder reads timestamp/frame_id/pose and skips the rest.
// ---------------------------------------------------------------------------

namespace {

// foxglove.Pose { position=1 Vector3 (x/y/z), orientation=2 Quaternion (x/y/z/w) }.
PW buildPoseSubmessage(const PoseValues& pv) {
  PW position;
  position.dbl(1, pv.px);
  position.dbl(2, pv.py);
  position.dbl(3, pv.pz);
  PW orientation;
  orientation.dbl(1, pv.qx);
  orientation.dbl(2, pv.qy);
  orientation.dbl(3, pv.qz);
  orientation.dbl(4, pv.qw);
  PW pose;
  pose.sub(1, position);
  pose.sub(2, orientation);
  return pose;
}

// Build a foxglove.Odometry message with velocities + a 36-element pose_covariance
// present, so the decoder is proven to skip every field that is not the pose.
PW buildOdometryWire(
    int64_t sec, int32_t ns, const std::string& frame_id, const std::string& body_frame_id, const PoseValues& pv) {
  PW linear_velocity;
  linear_velocity.dbl(1, 1.5);
  linear_velocity.dbl(2, 0.0);
  linear_velocity.dbl(3, 0.0);

  PW odom;
  odom.sub(1, foxgloveTimestamp(sec, ns));
  odom.str(2, frame_id);
  odom.str(3, body_frame_id);
  odom.sub(4, buildPoseSubmessage(pv));
  odom.sub(5, linear_velocity);                         // skipped (submessage)
  odom.packedDoubles(7, std::vector<double>(36, 0.0));  // skipped (packed repeated double)
  return odom;
}

}  // namespace

TEST(ProtobufParserTest, FoxgloveOdometryObjectRoute) {
  ProtobufParserFixture f;
  f.setUp();

  ASSERT_TRUE(f.bindSchema("foxglove.Odometry", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.Odometry", empty_schema), PJ::sdk::BuiltinObjectType::kPosesInFrame);

  const PoseValues pv{.px = 1.0, .py = 2.0, .pz = 3.0, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0};
  const auto wire = buildOdometryWire(5, 0, "odom", "base_link", pv);

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.b.data(), wire.b.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pf = std::any_cast<PJ::sdk::PosesInFrame>(&rec->object);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(pf->frame_id, "odom");  // reference frame (field 2), NOT body_frame_id
  EXPECT_EQ(pf->timestamp_ns, 5'000'000'000LL);
  EXPECT_FALSE(rec->ts.has_value());
  ASSERT_EQ(pf->poses.size(), 1u);  // pose at field 4; velocities + covariance skipped
  EXPECT_DOUBLE_EQ(pf->poses[0].position.x, 1.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.y, 2.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.z, 3.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].orientation.w, 1.0);
}

TEST(ProtobufParserTest, FoxgloveOdometryScalarRoute) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.Odometry", std::string{}));

  const PoseValues pv{.px = 1.0, .py = 2.0, .pz = 3.0, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0};
  const auto wire = buildOdometryWire(5, 0, "odom", "base_link", pv);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.b.data()), wire.b.size()), 555));

  // The scalar route keeps the single pose plottable as bounded per-axis columns
  // (mirroring the ROS Odometry scalar flatten); it never emits the velocities or
  // the 36-element covariances.
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  const auto* px = PJ::sdk::testing::ParserWriteRecorder::findField(row, "pose/position/x");
  ASSERT_NE(px, nullptr);
  EXPECT_DOUBLE_EQ(px->numeric, 1.0);
  const auto* qw = PJ::sdk::testing::ParserWriteRecorder::findField(row, "pose/orientation/w");
  ASSERT_NE(qw, nullptr);
  EXPECT_DOUBLE_EQ(qw->numeric, 1.0);

  // …plus the header pair every promoted schema now emits.
  const auto* timestamp = PJ::sdk::testing::ParserWriteRecorder::findField(row, "timestamp");
  ASSERT_NE(timestamp, nullptr);
  EXPECT_DOUBLE_EQ(timestamp->numeric, 5.0);

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "odom");  // the reference frame, not body_frame_id
}

// foxglove.Odometry with a NON-default numbering: official is { frame_id=2,
// body_frame_id=3, pose=4 }; here { pose=2, body_frame_id=3, frame_id=9 }. The
// decoder must follow the embedded descriptor, not the hardcoded numbers.
TEST(ProtobufParserTest, FoxgloveOdometryHonorsVariantSchemaFieldNumbers) {
  ProtobufParserFixture f;
  f.setUp();
  const std::string schema =
      buildFoxgloveSchema("Odometry", {{"timestamp", 1}, {"pose", 2}, {"body_frame_id", 3}, {"frame_id", 9}});
  ASSERT_TRUE(f.bindSchema("foxglove.Odometry", schema));

  const PoseValues pv{.px = 4.0, .py = 5.0, .pz = 6.0, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0};
  PW odom;
  odom.sub(1, foxgloveTimestamp(7, 0));
  odom.sub(2, buildPoseSubmessage(pv));  // pose (variant number)
  odom.str(3, "base_link");              // body_frame_id (variant number)
  odom.str(9, "world");                  // frame_id (variant number)

  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(odom.b.data(), odom.b.size()), {}};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();

  const auto* pf = std::any_cast<PJ::sdk::PosesInFrame>(&rec->object);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(pf->frame_id, "world");  // resolved from descriptor (field 9), not the default field 2
  ASSERT_EQ(pf->poses.size(), 1u);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.x, 4.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].position.z, 6.0);
  EXPECT_DOUBLE_EQ(pf->poses[0].orientation.w, 1.0);
}

// ---------------------------------------------------------------------------
// Scalar routes of the promoted Foxglove scene/image schemas.
//
// Every object-bearing schema emits its embedded sensor stamp (and frame_id
// where the schema has one) as scalar series, so a promoted topic still has a
// plottable time base: the ROW is filed under the host/log clock unless
// use_embedded_timestamp is on, which would otherwise leave the message's own
// stamp unreachable. These drive the real parse() path (bind → parse →
// recorder) rather than the codecs directly.
// ---------------------------------------------------------------------------

namespace {

// Runs one message through the scalar route and returns the recorded row.
const PJ::sdk::testing::RecordedRow* scalarRow(
    ProtobufParserFixture& f, std::string_view type_name, const std::vector<uint8_t>& wire) {
  if (!f.bindSchema(type_name, std::string{})) {
    return nullptr;
  }
  if (!f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555)) {
    return nullptr;
  }
  if (f.recorder.rows().size() != 1u) {
    return nullptr;
  }
  return &f.recorder.rows()[0];
}

void expectNumeric(const PJ::sdk::testing::RecordedRow& row, const std::string& name, double expected) {
  const auto* field = PJ::sdk::testing::ParserWriteRecorder::findField(row, name);
  ASSERT_NE(field, nullptr) << "missing scalar column: " << name;
  EXPECT_DOUBLE_EQ(field->numeric, expected) << name;
}

void expectString(const PJ::sdk::testing::RecordedRow& row, const std::string& name, const std::string& expected) {
  const auto* field = PJ::sdk::testing::ParserWriteRecorder::findField(row, name);
  ASSERT_NE(field, nullptr) << "missing scalar column: " << name;
  EXPECT_EQ(field->string_value, expected) << name;
}

}  // namespace

// The FrameTransform scalar route used to decode NOTHING and hardcode
// num_transforms=1 — an entirely synthetic row. It now reads the message (cheap:
// no blob) and reports the real stamp and count.
TEST(ProtobufParserTest, FoxgloveFrameTransformScalarRoute) {
  PW vec3;
  vec3.dbl(1, 1.0);
  vec3.dbl(2, 2.0);
  vec3.dbl(3, 3.0);
  PW quat;
  quat.dbl(4, 1.0);
  PW tf;
  tf.sub(1, foxgloveTimestamp(5, 250));
  tf.str(2, "world");
  tf.str(3, "base_link");
  tf.sub(4, vec3);
  tf.sub(5, quat);

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.FrameTransform", tf.b);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->timestamp, 555);  // embedded ts off by default → host clock
  expectNumeric(*row, "timestamp", static_cast<double>(5'000'000'250LL) * 1e-9);
  expectNumeric(*row, "num_transforms", 1.0);
}

TEST(ProtobufParserTest, FoxgloveCompressedImageScalarRoute) {
  const std::vector<uint8_t> blob = {0xFF, 0xD8, 0xFF, 0xE0, 0xDE, 0xAD};
  PW img;
  img.sub(1, foxgloveTimestamp(7, 42));
  img.bytesField(2, blob);
  img.str(3, "jpeg");
  img.str(4, "camera_optical");

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.CompressedImage", img.b);
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(7'000'000'042LL) * 1e-9);
  expectString(*row, "frame_id", "camera_optical");
  expectString(*row, "format", "jpeg");
  expectNumeric(*row, "data_size", static_cast<double>(blob.size()));
}

TEST(ProtobufParserTest, FoxgloveRawImageScalarRoute) {
  const std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 2x2 rgb8
  PW img;
  img.sub(1, foxgloveTimestamp(5, 0));
  img.str(2, "camera_optical");
  img.fixed32(3, 2);
  img.fixed32(4, 2);
  img.str(5, "rgb8");
  img.fixed32(6, 6);
  img.bytesField(7, pixels);

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.RawImage", img.b);
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", 5.0);
  expectString(*row, "frame_id", "camera_optical");
  expectNumeric(*row, "width", 2.0);
  expectNumeric(*row, "height", 2.0);
  expectNumeric(*row, "data_size", static_cast<double>(pixels.size()));
}

TEST(ProtobufParserTest, FoxgloveCameraCalibrationScalarRoute) {
  PW cc;
  cc.sub(1, foxgloveTimestamp(3, 500));
  cc.fixed32(2, 640);
  cc.fixed32(3, 480);
  cc.str(4, "plumb_bob");
  cc.packedDoubles(5, {0.1, 0.2, 0.3, 0.4, 0.5});
  cc.packedDoubles(6, {1, 0, 320, 0, 1, 240, 0, 0, 1});
  cc.str(9, "camera_optical");

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.CameraCalibration", cc.b);
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(3'000'000'500LL) * 1e-9);
  expectString(*row, "frame_id", "camera_optical");
  expectNumeric(*row, "width", 640.0);
  expectNumeric(*row, "height", 480.0);
}

// ImageAnnotations has no top-level frame_id (annotations are matched to an
// image by layer stacking); its stamp is adopted from the first annotation.
TEST(ProtobufParserTest, FoxgloveImageAnnotationsScalarRoute) {
  PW p0;
  p0.dbl(1, 10.0);
  p0.dbl(2, 20.0);
  PW pa;
  pa.sub(1, foxgloveTimestamp(8, 500));  // PointsAnnotation.timestamp
  pa.varint(2, 1);                       // type = POINTS
  pa.sub(3, p0);
  PW ann;
  ann.sub(2, pa);

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.ImageAnnotations", ann.b);
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(8'000'000'500LL) * 1e-9);
  expectNumeric(*row, "num_points", 1.0);
  expectNumeric(*row, "num_circles", 0.0);
  expectNumeric(*row, "num_texts", 0.0);
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(*row, "frame_id"), nullptr);
}

TEST(ProtobufParserTest, FoxgloveVoxelGridScalarRoute) {
  FoxgloveVoxelGridParams p;
  p.ts_sec = 4;
  p.ts_nanos = 250;
  p.frame_id = "map";
  p.cell_x = 0.05;
  p.cell_y = 0.05;
  p.cell_z = 0.05;
  p.row_count = 2;
  p.column_count = 2;
  p.cell_stride = 1;
  p.row_stride = 2;
  p.slice_stride = 4;
  p.fields = kVoxelFields;
  p.data = kVoxelData;

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.VoxelGrid", buildFoxgloveVoxelGridWire(p));
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(4'000'000'250LL) * 1e-9);
  expectString(*row, "frame_id", "map");
  expectNumeric(*row, "column_count", 2.0);
  expectNumeric(*row, "row_count", 2.0);
  expectNumeric(*row, "slice_count", 2.0);
  expectNumeric(*row, "data_size", static_cast<double>(kVoxelData.size()));
}

// foxglove.SceneUpdate has no TOP-LEVEL timestamp, but the first entity's stamp
// is the one the object route files the record under — so it is the stamp for
// the message, and the scalar route emits it like every other promoted schema.
// There is still no top-level frame_id (each entity carries its own).
TEST(ProtobufParserTest, FoxgloveSceneUpdateScalarRouteEmitsFirstEntityTimestamp) {
  PW entity;
  entity.sub(1, foxgloveTimestamp(3, 250));
  entity.str(2, "map");
  entity.str(3, "robot");
  PW scene;
  scene.sub(2, entity);

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.SceneUpdate", scene.b);
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(3'000'000'250LL) * 1e-9);
  expectNumeric(*row, "num_entities", 1.0);
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(*row, "frame_id"), nullptr);
  EXPECT_EQ(row->fields.size(), 2u);
}

// An empty SceneUpdate has no stamp anywhere: omit the column rather than file a
// fabricated 0.0 (same rule as foxglove.FrameTransform with no transforms).
TEST(ProtobufParserTest, FoxgloveSceneUpdateScalarRouteOmitsTimestampWhenEmpty) {
  PW scene;  // no entities at all

  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.SceneUpdate", scene.b);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(PJ::sdk::testing::ParserWriteRecorder::findField(*row, "timestamp"), nullptr);
  expectNumeric(*row, "num_entities", 0.0);
  EXPECT_EQ(row->fields.size(), 1u);
}

// With use_embedded_timestamp ON, the scalar ROW must land on the same clock the
// object route uses. These handlers previously left record.ts unset, so scalars
// stayed on host time while the object went to sensor time — the two views of
// one message disagreed about when it happened.
TEST(ProtobufParserTest, PromotedScalarRowsAdoptEmbeddedTimestamp) {
  const std::vector<uint8_t> blob = {0xFF, 0xD8, 0xFF, 0xE0};
  PW img;
  img.sub(1, foxgloveTimestamp(7, 42));
  img.bytesField(2, blob);
  img.str(3, "jpeg");
  img.str(4, "camera_optical");

  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.bindSchema("foxglove.CompressedImage", std::string{}));
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(img.b.data()), img.b.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 7'000'000'042LL) << "row must adopt the embedded stamp, not host time 555";
}

TEST(ProtobufParserTest, SceneUpdateScalarRowAdoptsEmbeddedTimestamp) {
  PW entity;
  entity.sub(1, foxgloveTimestamp(3, 250));
  entity.str(2, "map");
  PW scene;
  scene.sub(2, entity);

  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.bindSchema("foxglove.SceneUpdate", std::string{}));
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(scene.b.data()), scene.b.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 3'000'000'250LL);
}

TEST(ProtobufParserTest, VoxelGridScalarRowAdoptsEmbeddedTimestamp) {
  FoxgloveVoxelGridParams p;
  p.ts_sec = 4;
  p.ts_nanos = 250;
  p.frame_id = "map";
  p.cell_x = 0.05;
  p.cell_y = 0.05;
  p.cell_z = 0.05;
  p.row_count = 2;
  p.column_count = 2;
  p.cell_stride = 1;
  p.row_stride = 2;
  p.slice_stride = 4;
  p.fields = kVoxelFields;
  p.data = kVoxelData;
  const auto wire = buildFoxgloveVoxelGridWire(p);

  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.bindSchema("foxglove.VoxelGrid", std::string{}));
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 4'000'000'250LL);
}

// ---------------------------------------------------------------------------
// foxglove.Grid -> sdk::GridMap (zero-copy view)
// ---------------------------------------------------------------------------

namespace {

/// One foxglove.Grid message built by hand so tests can renumber every field
/// (parent and nested) and emit fields in any order.
struct FoxgloveGridWire {
  int64_t ts_sec = 7;
  int32_t ts_nanos = 250;
  std::string frame_id = "map";
  double origin_x = 1.0;
  double origin_y = 2.0;
  double origin_z = 0.5;
  uint32_t column_count = 3;
  double cell_x = 0.1;
  double cell_y = 0.2;
  uint32_t row_stride = 16;  // 3 x 4 bytes, padded
  uint32_t cell_stride = 4;
  std::vector<FoxgloveField> fields = {{"elevation", 0, 7}};
  std::vector<uint8_t> data = std::vector<uint8_t>(48, 0xAB);  // 3 rows
  pj_protobuf::GridFieldNumbers num;
};

/// The header half of a foxglove.Grid: timestamp, frame_id, pose, column_count, cell_size.
void appendGridHeader(PW& g, const FoxgloveGridWire& w) {
  const auto& n = w.num;
  g.sub(n.timestamp, foxgloveTimestamp(w.ts_sec, w.ts_nanos));
  g.str(n.frame_id, w.frame_id);
  g.sub(
      n.pose,
      buildPoseSubmessage(
          PoseValues{
              .px = w.origin_x, .py = w.origin_y, .pz = w.origin_z, .qx = 0.0, .qy = 0.0, .qz = 0.0, .qw = 1.0}));
  g.fixed32(n.column_count, w.column_count);
  PW cell;
  cell.dbl(1, w.cell_x);
  cell.dbl(2, w.cell_y);
  g.sub(n.cell_size, cell);
}

/// One PackedElementField submessage per entry of `w.fields`.
void appendGridFields(PW& g, const FoxgloveGridWire& w) {
  const auto& n = w.num;
  for (const auto& f : w.fields) {
    PW pef;
    pef.str(n.pef_name, f.name);
    pef.fixed32(n.pef_offset, f.offset);
    pef.varint(n.pef_type, static_cast<uint64_t>(f.numeric_type));
    g.sub(n.fields, pef);
  }
}

/// The official field order: header, strides, fields, data.
std::vector<uint8_t> buildFoxgloveGridWire(const FoxgloveGridWire& w) {
  const auto& n = w.num;
  PW g;
  appendGridHeader(g, w);
  g.fixed32(n.row_stride, w.row_stride);
  g.fixed32(n.cell_stride, w.cell_stride);
  appendGridFields(g, w);
  g.bytesField(n.data, w.data);
  return g.b;
}

/// Every field renumbered, parent and nested, in a self-describing schema.
pj_protobuf::GridFieldNumbers renumberedGridNumbers() {
  pj_protobuf::GridFieldNumbers n;
  n.timestamp = 9;
  n.frame_id = 8;
  n.pose = 7;
  n.column_count = 6;
  n.cell_size = 5;
  n.row_stride = 4;
  n.cell_stride = 3;
  n.fields = 2;
  n.data = 1;
  n.pef_name = 3;
  n.pef_offset = 1;
  n.pef_type = 2;
  return n;
}

std::string buildGridVariantSchema() {
  const auto n = renumberedGridNumbers();
  gp::FileDescriptorProto file;
  file.set_name("foxglove/Grid.proto");
  file.set_package("foxglove");
  file.set_syntax("proto3");
  auto* pef = file.add_message_type();
  pef->set_name("PackedElementField");
  addProtoField(pef, "name", n.pef_name, gp::FieldDescriptorProto::TYPE_STRING);
  addProtoField(pef, "offset", n.pef_offset, gp::FieldDescriptorProto::TYPE_FIXED32);
  addProtoField(pef, "type", n.pef_type, gp::FieldDescriptorProto::TYPE_INT32);
  auto* grid = file.add_message_type();
  grid->set_name("Grid");
  addProtoField(grid, "timestamp", n.timestamp, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(grid, "frame_id", n.frame_id, gp::FieldDescriptorProto::TYPE_STRING);
  addProtoField(grid, "pose", n.pose, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(grid, "column_count", n.column_count, gp::FieldDescriptorProto::TYPE_FIXED32);
  addProtoField(grid, "cell_size", n.cell_size, gp::FieldDescriptorProto::TYPE_BYTES);
  addProtoField(grid, "row_stride", n.row_stride, gp::FieldDescriptorProto::TYPE_FIXED32);
  addProtoField(grid, "cell_stride", n.cell_stride, gp::FieldDescriptorProto::TYPE_FIXED32);
  addProtoField(grid, "fields", n.fields, gp::FieldDescriptorProto::TYPE_MESSAGE, ".foxglove.PackedElementField", true);
  addProtoField(grid, "data", n.data, gp::FieldDescriptorProto::TYPE_BYTES);
  gp::FileDescriptorSet fds;
  *fds.add_file() = file;
  std::string out;
  fds.SerializeToString(&out);
  return out;
}

void expectGoldenGrid(
    const PJ::sdk::GridMap& g, const std::vector<uint8_t>& wire, const PJ::sdk::BufferAnchor& anchor) {
  EXPECT_EQ(g.timestamp_ns, 7'000'000'250LL);
  EXPECT_EQ(g.frame_id, "map");
  EXPECT_DOUBLE_EQ(g.origin.position.x, 1.0);
  EXPECT_DOUBLE_EQ(g.origin.position.y, 2.0);
  EXPECT_DOUBLE_EQ(g.origin.position.z, 0.5);
  EXPECT_DOUBLE_EQ(g.origin.orientation.w, 1.0);
  EXPECT_EQ(g.column_count, 3u);
  EXPECT_EQ(g.row_count, 3u);  // derived: 48 / 16
  EXPECT_DOUBLE_EQ(g.cell_size.x, 0.1);
  EXPECT_DOUBLE_EQ(g.cell_size.y, 0.2);
  EXPECT_EQ(g.row_stride, 16u);
  EXPECT_EQ(g.cell_stride, 4u);
  ASSERT_EQ(g.fields.size(), 1u);
  EXPECT_EQ(g.fields[0].name, "elevation");
  EXPECT_EQ(g.fields[0].offset, 0u);
  EXPECT_EQ(g.fields[0].datatype, PJ::sdk::PointField::Datatype::kFloat32);
  EXPECT_EQ(g.fields[0].count, 1u);
  // Zero-copy: the cell span aliases the wire buffer, and the anchor is forwarded.
  ASSERT_EQ(g.data.size(), 48u);
  EXPECT_GE(g.data.data(), wire.data());
  EXPECT_LE(g.data.data() + g.data.size(), wire.data() + wire.size());
  EXPECT_EQ(g.anchor, anchor);
  EXPECT_TRUE(PJ::validateGridMap(g).has_value());
}

}  // namespace

TEST(ProtobufParserTest, FoxgloveGridCodecDecodesZeroCopy) {
  const auto wire = buildFoxgloveGridWire(FoxgloveGridWire{});
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto decoded = pj_protobuf::deserializeFoxgloveGridView(wire.data(), wire.size(), anchor);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  expectGoldenGrid(*decoded, wire, anchor);
}

TEST(ProtobufParserTest, FoxgloveGridCodecAcceptsArbitraryFieldOrder) {
  // Arbitrary field order: bulk data and strides before the header and fields.
  const FoxgloveGridWire w;
  PW g;
  g.bytesField(w.num.data, w.data);
  g.fixed32(w.num.cell_stride, w.cell_stride);
  g.fixed32(w.num.row_stride, w.row_stride);
  appendGridHeader(g, w);
  appendGridFields(g, w);
  const auto& wire = g.b;
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  auto decoded = pj_protobuf::deserializeFoxgloveGridView(wire.data(), wire.size(), anchor);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  expectGoldenGrid(*decoded, wire, anchor);
}

TEST(ProtobufParserTest, FoxgloveGridCodecMapsAllEightDatatypes) {
  using D = PJ::sdk::PointField::Datatype;
  FoxgloveGridWire w;
  w.fields = {{"u8", 0, 1},  {"i8", 1, 2},   {"u16", 2, 3},  {"i16", 4, 4},
              {"u32", 6, 5}, {"i32", 10, 6}, {"f32", 14, 7}, {"f64", 18, 8}};
  w.cell_stride = 26;
  w.column_count = 2;
  w.row_stride = 52;
  w.data.assign(104, 0);
  const auto wire = buildFoxgloveGridWire(w);
  auto decoded = pj_protobuf::deserializeFoxgloveGridView(wire.data(), wire.size(), nullptr);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  ASSERT_EQ(decoded->fields.size(), 8u);
  const D expected[] = {D::kUint8, D::kInt8, D::kUint16, D::kInt16, D::kUint32, D::kInt32, D::kFloat32, D::kFloat64};
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(decoded->fields[i].datatype, expected[i]) << w.fields[i].name;
    EXPECT_EQ(decoded->fields[i].offset, w.fields[i].offset);
    EXPECT_EQ(decoded->fields[i].count, 1u);
  }
  EXPECT_EQ(decoded->row_count, 2u);
}

TEST(ProtobufParserTest, FoxgloveGridCodecRejectsBadLayouts) {
  auto decode = [](const FoxgloveGridWire& w) {
    const auto wire = buildFoxgloveGridWire(w);
    return pj_protobuf::deserializeFoxgloveGridView(wire.data(), wire.size(), nullptr);
  };
  FoxgloveGridWire unknown;
  unknown.fields = {{"elevation", 0, 0}};  // UNKNOWN
  EXPECT_FALSE(decode(unknown).has_value());
  unknown.fields = {{"elevation", 0, 9}};  // past the enum
  EXPECT_FALSE(decode(unknown).has_value());

  FoxgloveGridWire remainder;
  remainder.data.assign(50, 0);  // not a whole number of 16-byte rows
  EXPECT_FALSE(decode(remainder).has_value());

  FoxgloveGridWire zero_stride;
  zero_stride.row_stride = 0;
  EXPECT_FALSE(decode(zero_stride).has_value());

  FoxgloveGridWire overflow;
  overflow.column_count = 5;  // 5 x 4 > 16
  EXPECT_FALSE(decode(overflow).has_value());

  FoxgloveGridWire past_cell;
  past_cell.fields = {{"elevation", 2, 7}};  // f32 at offset 2 ends past cell_stride 4
  EXPECT_FALSE(decode(past_cell).has_value());

  // Empty data is a legal, zero-row grid.
  FoxgloveGridWire empty;
  empty.data.clear();
  auto ok = decode(empty);
  ASSERT_TRUE(ok.has_value()) << ok.error();
  EXPECT_EQ(ok->row_count, 0u);
}

TEST(ProtobufParserTest, FoxgloveGridCodecRejectsTruncatedMessages) {
  const auto wire = buildFoxgloveGridWire(FoxgloveGridWire{});
  // Cut inside the data bytes (the tail of the message) and inside the header.
  for (size_t cut : {wire.size() - 5, wire.size() - 47, size_t{20}}) {
    auto decoded = pj_protobuf::deserializeFoxgloveGridView(wire.data(), cut, nullptr);
    EXPECT_FALSE(decoded.has_value()) << "cut at " << cut;
  }
}

TEST(ProtobufParserTest, FoxgloveGridCodecRejectsNestedLengthPastTheEnd) {
  auto decode = [](const std::vector<uint8_t>& wire) {
    return pj_protobuf::deserializeFoxgloveGridView(wire.data(), wire.size(), nullptr);
  };
  const FoxgloveGridWire w;

  // A valid Grid followed by a Timestamp whose declared length exceeds the
  // remaining bytes: the nested limit must not be pushed past the buffer.
  auto overlong = buildFoxgloveGridWire(w);
  {
    PW g;
    g.tag(w.num.timestamp, 2);
    g.rawVarint(100);
    const PW ts = foxgloveTimestamp(7, 250);
    g.b.insert(g.b.end(), ts.b.begin(), ts.b.end());
    overlong.insert(overlong.end(), g.b.begin(), g.b.end());
  }
  EXPECT_FALSE(decode(overlong).has_value()) << "trailing Timestamp declares more bytes than remain";

  // A PackedElementField cut short: its declared length runs past the end.
  {
    PW g;
    appendGridHeader(g, w);
    g.fixed32(w.num.row_stride, w.row_stride);
    g.fixed32(w.num.cell_stride, w.cell_stride);
    g.bytesField(w.num.data, w.data);
    PW pef;
    pef.str(w.num.pef_name, "elevation");
    pef.fixed32(w.num.pef_offset, 0);
    pef.varint(w.num.pef_type, 7);
    g.tag(w.num.fields, 2);
    g.rawVarint(pef.b.size() + 8);
    g.b.insert(g.b.end(), pef.b.begin(), pef.b.end());
    EXPECT_FALSE(decode(g.b).has_value()) << "truncated PackedElementField";
  }

  // A nested message that ends early on a zero tag leaves bytes unconsumed.
  {
    PW g;
    PW ts;
    ts.b.push_back(0x00);
    g.sub(w.num.timestamp, ts);
    g.str(w.num.frame_id, w.frame_id);
    g.fixed32(w.num.column_count, w.column_count);
    g.fixed32(w.num.row_stride, w.row_stride);
    g.fixed32(w.num.cell_stride, w.cell_stride);
    appendGridFields(g, w);
    g.bytesField(w.num.data, w.data);
    EXPECT_FALSE(decode(g.b).has_value()) << "Timestamp not fully consumed";
  }

  // Trailing garbage after the last field is not a legitimate message end.
  auto trailing = buildFoxgloveGridWire(w);
  trailing.push_back(0x00);
  EXPECT_FALSE(decode(trailing).has_value()) << "top-level zero tag";
}

TEST(ProtobufParserTest, FoxgloveGridObjectRouteClassifiesAndForwardsAnchor) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.Grid", std::string{}));
  const PJ::Span<const uint8_t> empty_schema{};
  EXPECT_EQ(f.handle.classifySchema("foxglove.Grid", empty_schema), PJ::sdk::BuiltinObjectType::kGridMap);

  const auto wire = buildFoxgloveGridWire(FoxgloveGridWire{});
  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  ASSERT_NE(base, nullptr);
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();
  EXPECT_FALSE(rec->ts.has_value());  // use_embedded_timestamp off

  const auto* g = std::any_cast<PJ::sdk::GridMap>(&rec->object);
  ASSERT_NE(g, nullptr);
  expectGoldenGrid(*g, wire, anchor);
}

TEST(ProtobufParserTest, FoxgloveGridRenumberedSchemaResolvesParentAndNested) {
  ProtobufParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema("foxglove.Grid", buildGridVariantSchema()));
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));

  FoxgloveGridWire w;
  w.num = renumberedGridNumbers();
  const auto wire = buildFoxgloveGridWire(w);
  auto* base = static_cast<PJ::MessageParserPluginBase*>(f.handle.context());
  const PJ::sdk::BufferAnchor anchor = std::make_shared<std::vector<uint8_t>>();
  const PJ::sdk::PayloadView view{PJ::Span<const uint8_t>(wire.data(), wire.size()), anchor};
  auto rec = base->parseObject(1234, view);
  ASSERT_TRUE(rec.has_value()) << rec.error();
  ASSERT_TRUE(rec->ts.has_value());
  EXPECT_EQ(*rec->ts, 7'000'000'250LL);
  const auto* g = std::any_cast<PJ::sdk::GridMap>(&rec->object);
  ASSERT_NE(g, nullptr);
  expectGoldenGrid(*g, wire, anchor);

  // The official numbering must NOT decode against the renumbered binding.
  const auto official = buildFoxgloveGridWire(FoxgloveGridWire{});
  const PJ::sdk::PayloadView official_view{PJ::Span<const uint8_t>(official.data(), official.size()), anchor};
  auto wrong = base->parseObject(1234, official_view);
  EXPECT_TRUE(!wrong.has_value() || std::any_cast<PJ::sdk::GridMap>(&wrong->object)->frame_id != "map");
}

TEST(ProtobufParserTest, FoxgloveGridScalarRoute) {
  ProtobufParserFixture f;
  f.setUp();
  const auto* row = scalarRow(f, "foxglove.Grid", buildFoxgloveGridWire(FoxgloveGridWire{}));
  ASSERT_NE(row, nullptr);
  expectNumeric(*row, "timestamp", static_cast<double>(7'000'000'250LL) * 1e-9);
  expectString(*row, "frame_id", "map");
  expectNumeric(*row, "column_count", 3.0);
  expectNumeric(*row, "row_count", 3.0);
  expectNumeric(*row, "row_stride", 16.0);
  expectNumeric(*row, "cell_stride", 4.0);
  expectNumeric(*row, "data_size", 48.0);
  EXPECT_EQ(row->fields.size(), 7u);
}
