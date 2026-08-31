#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_ARROW_PARSER_PLUGIN_PATH
#error "PJ_ARROW_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

/// Host-side fixture that loads and binds the Arrow parser plugin DSO.
struct ArrowParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder recorder;

  /// Load the plugin, create a parser instance, and bind its required write service.
  void setUp() {
    auto loaded_library = PJ::MessageParserLibrary::load(PJ_ARROW_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(loaded_library) << loaded_library.error();
    library = std::move(*loaded_library);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(recorder.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }
};

/// Verify that plugin discovery advertises the Arrow IPC wire encoding.
TEST(ArrowParserTest, ManifestContainsArrowIpcEncoding) {
  ArrowParserFixture fixture;
  fixture.setUp();
  EXPECT_NE(fixture.handle.manifest().find("\"arrow-ipc\""), std::string::npos);
}

/// Verify that every schema is classified as a non-canonical object.
TEST(ArrowParserTest, ClassifiesSchemasAsNonCanonical) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const uint8_t schema_byte = 0;
  const PJ::Span<const uint8_t> schema(&schema_byte, 1);
  ASSERT_TRUE(fixture.handle.bindSchema("anything", schema));
  EXPECT_EQ(fixture.handle.classifySchema("anything", schema), PJ::sdk::BuiltinObjectType::kNone);
}

/// Verify that all supported configuration values survive load and save.
TEST(ArrowParserTest, ConfigRoundTrip) {
  ArrowParserFixture fixture;
  fixture.setUp();
  ASSERT_TRUE(fixture.handle.loadConfig(
      R"({"timestamp_column":"event_time","flatten_structs":false,"synthetic_interval_ns":2500000})"));

  std::string saved_config;
  ASSERT_TRUE(fixture.handle.saveConfig(saved_config));
  EXPECT_NE(saved_config.find("\"timestamp_column\":\"event_time\""), std::string::npos);
  EXPECT_NE(saved_config.find("\"flatten_structs\":false"), std::string::npos);
  EXPECT_NE(saved_config.find("\"synthetic_interval_ns\":2500000"), std::string::npos);
}

/// Verify that an untouched parser saves every documented default.
TEST(ArrowParserTest, ConfigDefaultsMatchContract) {
  ArrowParserFixture fixture;
  fixture.setUp();

  std::string saved_config;
  ASSERT_TRUE(fixture.handle.saveConfig(saved_config));
  EXPECT_NE(saved_config.find("\"timestamp_column\":\"\""), std::string::npos);
  EXPECT_NE(saved_config.find("\"flatten_structs\":true"), std::string::npos);
  EXPECT_NE(saved_config.find("\"synthetic_interval_ns\":0"), std::string::npos);
}

/// Verify that malformed JSON is reported through the parser Status API.
TEST(ArrowParserTest, MalformedConfigReturnsError) {
  ArrowParserFixture fixture;
  fixture.setUp();
  EXPECT_FALSE(fixture.handle.loadConfig("{"));
}

/// Verify that the Task 1 parser stub rejects payloads without crashing.
TEST(ArrowParserTest, ParseReturnsNotImplementedError) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const uint8_t payload_byte = 0;
  const auto status = fixture.handle.parse(1000, PJ::Span<const uint8_t>(&payload_byte, 1));
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("parser_arrow: not implemented"), std::string::npos);
}

}  // namespace
