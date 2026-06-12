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

#include "../foxglove_object_codecs.hpp"
#include "../foxglove_pointcloud_codec.hpp"
#include "pj_base/builtin/builtin_object.hpp"
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

  const auto wire = buildVideoFrameWire("h265", 0);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

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

}  // namespace

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

  const auto wire = buildFoxglovePointCloudWire(7, 0, "lidar_top", 16, kCloudFields, kCloudBlob, false, 0.0);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

  const auto* frame_id = PJ::sdk::testing::ParserWriteRecorder::findField(row, "frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->string_value, "lidar_top");

  const auto* point_count = PJ::sdk::testing::ParserWriteRecorder::findField(row, "point_count");
  ASSERT_NE(point_count, nullptr);
  EXPECT_DOUBLE_EQ(point_count->numeric, 2.0);

  const auto* point_step = PJ::sdk::testing::ParserWriteRecorder::findField(row, "point_step");
  ASSERT_NE(point_step, nullptr);
  EXPECT_DOUBLE_EQ(point_step->numeric, 16.0);

  const auto* num_fields = PJ::sdk::testing::ParserWriteRecorder::findField(row, "num_fields");
  ASSERT_NE(num_fields, nullptr);
  EXPECT_DOUBLE_EQ(num_fields->numeric, 4.0);
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

  const auto wire = buildFoxgloveLaserScanWire(7, 0, "lidar_2d", -0.5, 0.5, {1.0, kDNaN, 2.0}, {}, false, 0.0);
  ASSERT_TRUE(f.parse(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()), 555));

  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& row = f.recorder.rows()[0];
  EXPECT_EQ(row.timestamp, 555);  // embedded ts disabled by default → host ts

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
