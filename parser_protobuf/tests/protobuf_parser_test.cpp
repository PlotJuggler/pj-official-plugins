#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>
#include <gtest/gtest.h>

#include <any>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

}  // namespace
