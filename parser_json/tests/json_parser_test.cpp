#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_JSON_PARSER_PLUGIN_PATH
#error "PJ_JSON_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct JsonParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_JSON_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(recorder.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  bool parse(std::string_view json, int64_t ts = 1000) {
    const auto* data = reinterpret_cast<const uint8_t*>(json.data());
    return handle.parse(ts, PJ::Span<const uint8_t>(data, json.size())).has_value();
  }
};

TEST(JsonParserTest, FlatObject) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"temperature":23.5,"pressure":1013.25})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields.size(), 2u);

  bool found_temp = false;
  bool found_press = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "temperature") {
      EXPECT_DOUBLE_EQ(field.numeric, 23.5);
      found_temp = true;
    } else if (field.name == "pressure") {
      EXPECT_DOUBLE_EQ(field.numeric, 1013.25);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 2u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "sensor/x");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 1.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[1].name, "sensor/y");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[1].numeric, 2.0);
}

TEST(JsonParserTest, ArrayExpansion) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"vec":[10,20,30]})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 3u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "vec[0]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 10.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[1].name, "vec[1]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[1].numeric, 20.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[2].name, "vec[2]");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[2].numeric, 30.0);
}

TEST(JsonParserTest, BooleanFields) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"active":true,"error":false})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 2u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "active");
  EXPECT_EQ(f.recorder.rows()[0].fields[0].type, PJ::PrimitiveType::kBool);
  EXPECT_TRUE(f.recorder.rows()[0].fields[0].bool_value);
  EXPECT_EQ(f.recorder.rows()[0].fields[1].name, "error");
  EXPECT_EQ(f.recorder.rows()[0].fields[1].type, PJ::PrimitiveType::kBool);
  EXPECT_FALSE(f.recorder.rows()[0].fields[1].bool_value);
}

TEST(JsonParserTest, StringFieldsIncluded) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"name":"sensor1","value":42.0})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 2u);
  bool found_name = false;
  bool found_value = false;
  for (const auto& field : f.recorder.rows()[0].fields) {
    if (field.name == "name") {
      EXPECT_EQ(field.type, PJ::PrimitiveType::kString);
      EXPECT_EQ(field.string_value, "sensor1");
      found_name = true;
    } else if (field.name == "value") {
      found_value = true;
    }
  }
  EXPECT_TRUE(found_name);
  EXPECT_TRUE(found_value);
}

// Regression: several short (SSO-range) string values in one payload must
// not corrupt each other. The internal string buffer is a std::deque
// precisely so push_back doesn't invalidate string_view references captured
// before it — a std::vector here would dangle prior views on each realloc.
TEST(JsonParserTest, MultipleShortStringsPreserved) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"a":"hi","b":"bye","c":"ok","d":"yes","e":"no","f":"end"})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 6u);
  const std::vector<std::pair<std::string, std::string>> expected{{"a", "hi"},  {"b", "bye"}, {"c", "ok"},
                                                                  {"d", "yes"}, {"e", "no"},  {"f", "end"}};
  for (const auto& [name, value] : expected) {
    bool found = false;
    for (const auto& field : f.recorder.rows()[0].fields) {
      if (field.name == name) {
        EXPECT_EQ(field.type, PJ::PrimitiveType::kString);
        EXPECT_EQ(field.string_value, value) << "field " << name;
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "field " << name << " missing";
  }
}

TEST(JsonParserTest, NullFieldsSkipped) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"a":null,"b":5.0})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "b");
}

TEST(JsonParserTest, RootArrayFlattened) {
  // parse_scalars yields one ScalarRecord per message, so a root JSON array
  // flattens into a single row with index-prefixed field names.
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"([{"v":1.0},{"v":2.0},{"v":3.0}])"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 3u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "[0]/v");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 1.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[1].name, "[1]/v");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[1].numeric, 2.0);
  EXPECT_EQ(f.recorder.rows()[0].fields[2].name, "[2]/v");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[2].numeric, 3.0);
}

TEST(JsonParserTest, DeeplyNested) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({"a":{"b":{"c":99.0}}})"));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  ASSERT_EQ(f.recorder.rows()[0].fields.size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields[0].name, "a/b/c");
  EXPECT_DOUBLE_EQ(f.recorder.rows()[0].fields[0].numeric, 99.0);
}

TEST(JsonParserTest, EmptyObject) {
  // An empty JSON object has no scalar fields; appendRecord with an empty
  // field span is a no-op in the datastore, so no row is recorded.
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.parse(R"({})"));
  EXPECT_EQ(f.recorder.rows().size(), 0u);
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
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 12345);
}

TEST(JsonParserTest, ManifestContainsEncoding) {
  JsonParserFixture f;
  f.setUp();
  // Manifest uses "encoding" as an array containing all supported encodings
  EXPECT_NE(f.handle.manifest().find("\"json\""), std::string::npos);
  EXPECT_NE(f.handle.manifest().find("\"cbor\""), std::string::npos);
  EXPECT_NE(f.handle.manifest().find("\"msgpack\""), std::string::npos);
  EXPECT_NE(f.handle.manifest().find("\"bson\""), std::string::npos);
}

// ---------------------------------------------------------------------------
// Embedded timestamp tests
// ---------------------------------------------------------------------------

TEST(JsonParserTest, EmbeddedTimestampDisabledByDefault) {
  JsonParserFixture f;
  f.setUp();
  // Without config, the host timestamp should be used even if JSON has "timestamp" field
  ASSERT_TRUE(f.parse(R"({"timestamp":1234.567,"value":42.0})", 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 9999);  // host timestamp, not embedded
}

TEST(JsonParserTest, EmbeddedTimestampEnabled) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  ASSERT_TRUE(f.parse(R"({"timestamp":1234.567,"value":42.0})", 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  // 1234.567 seconds → nanoseconds
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 1234567000000LL);
}

TEST(JsonParserTest, EmbeddedTimestampCustomFieldName) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true,"timestamp_field_name":"ts"})"));
  ASSERT_TRUE(f.parse(R"({"ts":5678.123,"value":42.0})", 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 5678123000000LL);
}

TEST(JsonParserTest, EmbeddedTimestampMissingFieldFallsBackToHost) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  // JSON doesn't have "timestamp" field, should use host timestamp
  ASSERT_TRUE(f.parse(R"({"value":42.0})", 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 9999);
}

TEST(JsonParserTest, EmbeddedTimestampIntegerValue) {
  JsonParserFixture f;
  f.setUp();
  ASSERT_TRUE(f.handle.loadConfig(R"({"use_embedded_timestamp":true})"));
  // Integer timestamp (seconds)
  ASSERT_TRUE(f.parse(R"({"timestamp":1000,"value":42.0})", 9999));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 1000000000000LL);  // 1000s * 1e9
}

}  // namespace
