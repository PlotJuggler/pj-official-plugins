#include "pj_plugins/host/message_parser_library.hpp"

#include <gtest/gtest.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/reflection.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/plugin_data_api.h"

#ifndef PJ_PROTOBUF_PARSER_PLUGIN_PATH
#error "PJ_PROTOBUF_PARSER_PLUGIN_PATH must be defined"
#endif

namespace gp = google::protobuf;

namespace {

struct RecordedField {
  std::string name;
  double value = 0.0;
  std::string string_value;
  bool is_string = false;
};

struct RecordedRow {
  int64_t timestamp = 0;
  std::vector<RecordedField> fields;
};

struct ParserWriteRecorder {
  std::vector<RecordedRow> rows;
  std::string last_error;
  std::unordered_map<uint32_t, std::string> field_names;
  uint32_t next_field_id = 0;

  static const char* getLastError(void* ctx) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    return self->last_error.empty() ? nullptr : self->last_error.c_str();
  }

  static bool ensureField(void* ctx, PJ_string_view_t name, PJ_primitive_type_t,
                           PJ_field_handle_t* out_field) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    uint32_t id = self->next_field_id++;
    self->field_names[id] = std::string(name.data, name.size);
    *out_field = PJ_field_handle_t{{1}, id};
    return true;
  }

  static RecordedField extractField(const PJ_scalar_value_t& value, const std::string& name) {
    RecordedField f;
    f.name = name;
    switch (value.type) {
      case PJ_PRIMITIVE_TYPE_FLOAT64:
        f.value = value.data.as_float64;
        break;
      case PJ_PRIMITIVE_TYPE_FLOAT32:
        f.value = static_cast<double>(value.data.as_float32);
        break;
      case PJ_PRIMITIVE_TYPE_INT32:
        f.value = static_cast<double>(value.data.as_int32);
        break;
      case PJ_PRIMITIVE_TYPE_INT64:
        f.value = static_cast<double>(value.data.as_int64);
        break;
      case PJ_PRIMITIVE_TYPE_UINT32:
        f.value = static_cast<double>(value.data.as_uint32);
        break;
      case PJ_PRIMITIVE_TYPE_UINT64:
        f.value = static_cast<double>(value.data.as_uint64);
        break;
      case PJ_PRIMITIVE_TYPE_BOOL:
        f.value = value.data.as_bool != 0 ? 1.0 : 0.0;
        break;
      case PJ_PRIMITIVE_TYPE_STRING:
        if (value.data.as_string.data != nullptr) {
          f.string_value = std::string(value.data.as_string.data, value.data.as_string.size);
          f.is_string = true;
        }
        break;
      default:
        break;
    }
    return f;
  }

  static bool appendRecord(void* ctx, int64_t timestamp, const PJ_named_field_value_t* fields,
                            size_t field_count) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    RecordedRow row;
    row.timestamp = timestamp;
    for (size_t i = 0; i < field_count; ++i) {
      std::string name(fields[i].name.data, fields[i].name.size);
      row.fields.push_back(extractField(fields[i].value, name));
    }
    self->rows.push_back(std::move(row));
    return true;
  }

  static bool appendBoundRecord(void* ctx, int64_t timestamp, const PJ_bound_field_value_t* fields,
                                 size_t field_count) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    RecordedRow row;
    row.timestamp = timestamp;
    for (size_t i = 0; i < field_count; ++i) {
      auto it = self->field_names.find(fields[i].field.id);
      std::string name = (it != self->field_names.end()) ? it->second : "unknown";
      row.fields.push_back(extractField(fields[i].value, name));
    }
    self->rows.push_back(std::move(row));
    return true;
  }

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
  ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_PROTOBUF_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(handle.bindWriteHost(makeWriteHost(&recorder)));
  }

  bool bindSchema(std::string_view type_name, const std::string& schema_bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(schema_bytes.data());
    return handle.bindSchema(type_name, {data, schema_bytes.size()});
  }

  bool parse(const std::string& serialized, int64_t ts = 1000) {
    const auto* data = reinterpret_cast<const uint8_t*>(serialized.data());
    return handle.parse(ts, {data, serialized.size()});
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

  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 2u);

  bool found_temp = false;
  bool found_status = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "temperature") {
      EXPECT_DOUBLE_EQ(field.value, 23.5);
      found_temp = true;
    } else if (field.name == "status") {
      EXPECT_DOUBLE_EQ(field.value, 42.0);
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
  gp::Message* header_msg =
      ref->MutableMessage(msg.get(), stamped_desc->FindFieldByName("header"));
#pragma pop_macro("GetMessage")
  const gp::Reflection* hdr_ref = header_msg->GetReflection();
  hdr_ref->SetInt32(header_msg, header_msg->GetDescriptor()->FindFieldByName("seq"), 7);
  ref->SetDouble(msg.get(), stamped_desc->FindFieldByName("value"), 3.14);

  std::string payload;
  msg->SerializeToString(&payload);

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  bool found_seq = false;
  bool found_val = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "header/seq") {
      EXPECT_DOUBLE_EQ(field.value, 7.0);
      found_seq = true;
    } else if (field.name == "value") {
      EXPECT_DOUBLE_EQ(field.value, 3.14);
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
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 4u);

  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "values[0]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 1.0);
  EXPECT_EQ(f.recorder.rows[0].fields[1].name, "values[1]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[1].value, 2.0);
  EXPECT_EQ(f.recorder.rows[0].fields[2].name, "values[2]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[2].value, 3.0);
  EXPECT_EQ(f.recorder.rows[0].fields[3].name, "count");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[3].value, 3.0);
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
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  bool found_color = false;
  bool found_val = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "color") {
      EXPECT_TRUE(field.is_string);
      EXPECT_EQ(field.string_value, "GREEN");
      found_color = true;
    } else if (field.name == "value") {
      EXPECT_DOUBLE_EQ(field.value, 99.0);
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
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].timestamp, 99999);
}

TEST(ProtobufParserTest, ManifestContainsEncoding) {
  ProtobufParserFixture f;
  f.setUp();
  EXPECT_NE(f.handle.manifest().find("\"encoding\": \"protobuf\""), std::string::npos);
}

}  // namespace
