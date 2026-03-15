#include "pj_plugins/host/message_parser_library.hpp"

#include <gtest/gtest.h>

#include <data_tamer_parser/data_tamer_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"

#ifndef PJ_DATA_TAMER_PARSER_PLUGIN_PATH
#error "PJ_DATA_TAMER_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct RecordedField {
  std::string name;
  double value = 0.0;
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

  static bool appendRecord(void* ctx, int64_t timestamp, const PJ_named_field_value_t* fields,
                           size_t field_count) {
    auto* self = static_cast<ParserWriteRecorder*>(ctx);
    RecordedRow row;
    row.timestamp = timestamp;
    for (size_t i = 0; i < field_count; ++i) {
      RecordedField f;
      f.name = std::string(fields[i].name.data, fields[i].name.size);
      if (fields[i].value.type == PJ_PRIMITIVE_TYPE_FLOAT64) {
        f.value = fields[i].value.data.as_float64;
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_FLOAT32) {
        f.value = static_cast<double>(fields[i].value.data.as_float32);
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_INT32) {
        f.value = static_cast<double>(fields[i].value.data.as_int32);
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_UINT32) {
        f.value = static_cast<double>(fields[i].value.data.as_uint32);
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

struct DTFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_DATA_TAMER_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(handle.bindWriteHost(makeWriteHost(&recorder)));
  }

  bool bindSchema(const std::string& schema_text) {
    const auto* data = reinterpret_cast<const uint8_t*>(schema_text.data());
    return handle.bindSchema("data_tamer", {data, schema_text.size()});
  }

  bool parse(const std::vector<uint8_t>& payload, int64_t ts = 1000) {
    return handle.parse(ts, {payload.data(), payload.size()});
  }
};

const RecordedField* findField(const RecordedRow& row, const std::string& name) {
  for (const auto& f : row.fields) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

// Build a DataTamer binary snapshot payload:
//   uint32_t mask_size | active_mask bytes | uint32_t payload_size | payload bytes
std::vector<uint8_t> buildSnapshot(const DataTamerParser::Schema& schema,
                                   const std::vector<double>& values) {
  // All fields active: mask = all 1s.
  size_t num_fields = schema.fields.size();
  size_t mask_bytes = (num_fields + 7) / 8;
  std::vector<uint8_t> mask(mask_bytes, 0xFF);

  // Serialize payload: all values as their declared types.
  std::vector<uint8_t> payload_buf;
  for (size_t i = 0; i < std::min(num_fields, values.size()); i++) {
    const auto& field = schema.fields[i];
    switch (field.type) {
      case DataTamerParser::BasicType::FLOAT64: {
        double v = values[i];
        auto* p = reinterpret_cast<const uint8_t*>(&v);
        payload_buf.insert(payload_buf.end(), p, p + sizeof(v));
        break;
      }
      case DataTamerParser::BasicType::INT32: {
        auto v = static_cast<int32_t>(values[i]);
        auto* p = reinterpret_cast<const uint8_t*>(&v);
        payload_buf.insert(payload_buf.end(), p, p + sizeof(v));
        break;
      }
      case DataTamerParser::BasicType::FLOAT32: {
        auto v = static_cast<float>(values[i]);
        auto* p = reinterpret_cast<const uint8_t*>(&v);
        payload_buf.insert(payload_buf.end(), p, p + sizeof(v));
        break;
      }
      default: {
        double v = values[i];
        auto* p = reinterpret_cast<const uint8_t*>(&v);
        payload_buf.insert(payload_buf.end(), p, p + sizeof(v));
        break;
      }
    }
  }

  // Assemble: mask_size(4) + mask + payload_size(4) + payload
  std::vector<uint8_t> result;
  auto push32 = [&](uint32_t val) {
    auto* p = reinterpret_cast<const uint8_t*>(&val);
    result.insert(result.end(), p, p + 4);
  };
  push32(static_cast<uint32_t>(mask_bytes));
  result.insert(result.end(), mask.begin(), mask.end());
  push32(static_cast<uint32_t>(payload_buf.size()));
  result.insert(result.end(), payload_buf.begin(), payload_buf.end());
  return result;
}

// ---- Tests ----

TEST(DataTamerParserTest, ManifestContainsEncoding) {
  DTFixture f;
  f.setUp();
  EXPECT_NE(f.handle.manifest().find("\"encoding\":\"data_tamer\""), std::string::npos);
}

TEST(DataTamerParserTest, BasicParsing) {
  DTFixture f;
  f.setUp();

  std::string schema_text = "float64 temperature\nfloat64 pressure\n";
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuilSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {23.5, 101.3});

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  auto* temp = findField(f.recorder.rows[0], "/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->value, 23.5);

  auto* pres = findField(f.recorder.rows[0], "/pressure");
  ASSERT_NE(pres, nullptr);
  EXPECT_DOUBLE_EQ(pres->value, 101.3);
}

TEST(DataTamerParserTest, MixedTypes) {
  DTFixture f;
  f.setUp();

  std::string schema_text = "int32 count\nfloat32 ratio\nfloat64 value\n";
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuilSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {42.0, 0.5, 3.14});

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows.size(), 1u);

  auto* count = findField(f.recorder.rows[0], "/count");
  ASSERT_NE(count, nullptr);
  EXPECT_DOUBLE_EQ(count->value, 42.0);

  auto* ratio = findField(f.recorder.rows[0], "/ratio");
  ASSERT_NE(ratio, nullptr);
  EXPECT_NEAR(ratio->value, 0.5, 1e-6);

  auto* val = findField(f.recorder.rows[0], "/value");
  ASSERT_NE(val, nullptr);
  EXPECT_DOUBLE_EQ(val->value, 3.14);
}

TEST(DataTamerParserTest, TimestampPreserved) {
  DTFixture f;
  f.setUp();

  std::string schema_text = "float64 x\n";
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuilSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {1.0});

  ASSERT_TRUE(f.parse(payload, 99999));
  EXPECT_EQ(f.recorder.rows[0].timestamp, 99999);
}

TEST(DataTamerParserTest, EmptyPayload) {
  DTFixture f;
  f.setUp();

  std::string schema_text = "float64 x\n";
  ASSERT_TRUE(f.bindSchema(schema_text));

  // All fields inactive: mask = all 0s.
  std::vector<uint8_t> payload;
  auto push32 = [&](uint32_t val) {
    auto* p = reinterpret_cast<const uint8_t*>(&val);
    payload.insert(payload.end(), p, p + 4);
  };
  push32(1);           // mask_size = 1
  payload.push_back(0);  // mask = 0 (field inactive)
  push32(0);           // payload_size = 0

  ASSERT_TRUE(f.parse(payload));
  // No fields emitted since the field is inactive.
  EXPECT_EQ(f.recorder.rows.size(), 0u);
}

}  // namespace
