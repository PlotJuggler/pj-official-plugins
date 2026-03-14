#include "pj_plugins/host/message_parser_library.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "pj_base/plugin_data_api.h"

#ifndef PJ_JSON_PARSER_PLUGIN_PATH
#error "PJ_JSON_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct RecordedField {
  std::string name;
  double value = 0.0;
  bool is_bool = false;
  bool bool_value = false;
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

  static bool ensureField(void*, PJ_string_view_t, PJ_primitive_type_t,
                           PJ_field_handle_t* out_field) {
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
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_INT64) {
        f.value = static_cast<double>(fields[i].value.data.as_int64);
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_UINT64) {
        f.value = static_cast<double>(fields[i].value.data.as_uint64);
      } else if (fields[i].value.type == PJ_PRIMITIVE_TYPE_BOOL) {
        f.is_bool = true;
        f.bool_value = fields[i].value.data.as_bool != 0;
      }
      row.fields.push_back(f);
    }
    self->rows.push_back(std::move(row));
    return true;
  }

  static bool appendBoundRecord(void*, int64_t, const PJ_bound_field_value_t*, size_t) {
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

struct JsonParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_JSON_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    ASSERT_TRUE(handle.bindWriteHost(makeWriteHost(&recorder)));
  }

  bool parse(std::string_view json, int64_t ts = 1000) {
    const auto* data = reinterpret_cast<const uint8_t*>(json.data());
    return handle.parse(ts, {data, json.size()});
  }
};

TEST(JsonParserTest, FlatObject) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"temperature":23.5,"pressure":1013.25})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields.size(), 2u);

  bool found_temp = false;
  bool found_press = false;
  for (const auto& field : f.recorder.rows[0].fields) {
    if (field.name == "temperature") {
      EXPECT_DOUBLE_EQ(field.value, 23.5);
      found_temp = true;
    } else if (field.name == "pressure") {
      EXPECT_DOUBLE_EQ(field.value, 1013.25);
      found_press = true;
    }
  }
  EXPECT_TRUE(found_temp);
  EXPECT_TRUE(found_press);
}

TEST(JsonParserTest, NestedObject) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"sensor":{"x":1.0,"y":2.0}})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 2u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "sensor/x");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 1.0);
  EXPECT_EQ(f.recorder.rows[0].fields[1].name, "sensor/y");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[1].value, 2.0);
}

TEST(JsonParserTest, ArrayExpansion) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"vec":[10,20,30]})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 3u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "vec[0]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 10.0);
  EXPECT_EQ(f.recorder.rows[0].fields[1].name, "vec[1]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[1].value, 20.0);
  EXPECT_EQ(f.recorder.rows[0].fields[2].name, "vec[2]");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[2].value, 30.0);
}

TEST(JsonParserTest, BooleanFields) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"active":true,"error":false})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 2u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "active");
  EXPECT_TRUE(f.recorder.rows[0].fields[0].is_bool);
  EXPECT_TRUE(f.recorder.rows[0].fields[0].bool_value);
  EXPECT_EQ(f.recorder.rows[0].fields[1].name, "error");
  EXPECT_TRUE(f.recorder.rows[0].fields[1].is_bool);
  EXPECT_FALSE(f.recorder.rows[0].fields[1].bool_value);
}

TEST(JsonParserTest, StringFieldsSkipped) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"name":"sensor1","value":42.0})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "value");
}

TEST(JsonParserTest, NullFieldsSkipped) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"a":null,"b":5.0})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "b");
}

TEST(JsonParserTest, BatchedArray) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"([{"v":1.0},{"v":2.0},{"v":3.0}])"));
  ASSERT_EQ(f.recorder.rows.size(), 3u);
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 1.0);
  EXPECT_DOUBLE_EQ(f.recorder.rows[1].fields[0].value, 2.0);
  EXPECT_DOUBLE_EQ(f.recorder.rows[2].fields[0].value, 3.0);
}

TEST(JsonParserTest, DeeplyNested) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"a":{"b":{"c":99.0}}})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  ASSERT_EQ(f.recorder.rows[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields[0].name, "a/b/c");
  EXPECT_DOUBLE_EQ(f.recorder.rows[0].fields[0].value, 99.0);
}

TEST(JsonParserTest, EmptyObject) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({})"));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].fields.size(), 0u);
}

TEST(JsonParserTest, InvalidJsonFails) {
  JsonParserFixture f;
  f.setUp();
  EXPECT_FALSE(f.parse("not json at all"));
}

TEST(JsonParserTest, TimestampPreserved) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"v":1.0})", 12345));
  ASSERT_EQ(f.recorder.rows.size(), 1u);
  EXPECT_EQ(f.recorder.rows[0].timestamp, 12345);
}

TEST(JsonParserTest, ManifestContainsEncoding) {
  JsonParserFixture f;
  f.setUp();
  EXPECT_NE(f.handle.manifest().find("\"encoding\":\"json\""), std::string::npos);
}

}  // namespace
