#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arrow_write_host_fake.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_runtime_recorder.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "test_utils.hpp"

#ifndef PJ_ARROW_PARSER_PLUGIN_PATH
#error "PJ_ARROW_PARSER_PLUGIN_PATH must be defined"
#endif

#ifndef PJ_ARROW_TEST_DATA_DIR
#error "PJ_ARROW_TEST_DATA_DIR must be defined"
#endif

namespace {

/// Host-side fixture that loads and binds the Arrow parser plugin DSO.
struct ArrowParserFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  pj::parser_arrow::test::ArrowWriteHostFake arrow_write_host;
  PJ::sdk::testing::ParserWriteRecorder legacy_write_host;
  PJ::sdk::testing::ParserRuntimeRecorder runtime_host;

  /// Load the plugin, create a parser instance, and bind the selected host services.
  void setUp(bool arrow_capable = true, bool bind_runtime = false) {
    auto loaded_library = PJ::MessageParserLibrary::load(PJ_ARROW_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(loaded_library) << loaded_library.error();
    library = std::move(*loaded_library);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    if (arrow_capable) {
      registry.registerService<PJ::sdk::ParserWriteHostService>(arrow_write_host.makeHost());
    } else {
      registry.registerService<PJ::sdk::ParserWriteHostService>(legacy_write_host.makeHost());
    }
    if (bind_runtime) {
      registry.registerService<PJ::sdk::ParserRuntimeHostService>(runtime_host.makeHost());
    }
    ASSERT_TRUE(handle.bind(registry.view()));
  }
};

/// Return the absolute path of one checked-in Arrow IPC fixture.
[[nodiscard]] std::filesystem::path fixturePath(std::string_view filename) {
  return std::filesystem::path(PJ_ARROW_TEST_DATA_DIR) / filename;
}

/// Read and parse one checked-in Arrow IPC fixture.
[[nodiscard]] PJ::Status parseFixture(
    ArrowParserFixture& fixture, std::string_view filename, PJ::Timestamp timestamp_ns = 0) {
  const auto payload = pj::parser_arrow::test::readFile(fixturePath(filename));
  return fixture.handle.parse(timestamp_ns, PJ::Span<const uint8_t>(payload.data(), payload.size()));
}

/// Return the recorded format for one schema child name.
[[nodiscard]] std::string_view formatFor(
    const pj::parser_arrow::test::RecordedArrowStream& stream, std::string_view name) {
  const auto found = std::find(stream.schema_names.begin(), stream.schema_names.end(), name);
  if (found == stream.schema_names.end()) {
    return {};
  }
  const auto index = static_cast<std::size_t>(std::distance(stream.schema_names.begin(), found));
  return stream.schema_formats[index];
}

/// Assert that the parser used only the Arrow bulk path.
void expectOnlyArrowWrites(const pj::parser_arrow::test::ArrowWriteHostFake& write_host, uint64_t expected_calls) {
  EXPECT_EQ(write_host.ensureFieldCalls(), 0);
  EXPECT_EQ(write_host.appendRecordCalls(), 0);
  EXPECT_EQ(write_host.appendBoundRecordCalls(), 0);
  EXPECT_EQ(write_host.appendArrowStreamCalls(), expected_calls);
}

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

/// A flat Arrow IPC payload reaches the host with its schema, rows, and timestamps intact.
TEST(ArrowParserTest, ParsesFlatStreamThroughArrowHostPath) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const auto status = parseFixture(fixture, "flat.arrows");
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  const auto& stream = fixture.arrow_write_host.streams()[0];
  EXPECT_EQ(stream.timestamp_column, "timestamp_ns");
  EXPECT_EQ(stream.schema_names, (std::vector<std::string>{"timestamp_ns", "a", "b", "name"}));
  EXPECT_EQ(stream.batch_row_counts, (std::vector<int64_t>{3}));
  EXPECT_EQ(stream.timestamp_values, (std::vector<int64_t>{1000, 2000, 3000}));
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// Record-batch boundaries survive decoding and shaping.
TEST(ArrowParserTest, PreservesRecordBatchBoundaries) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const auto status = parseFixture(fixture, "flat_two_batches.arrows");
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  EXPECT_EQ(fixture.arrow_write_host.streams()[0].batch_row_counts, (std::vector<int64_t>{2, 1}));
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// Default shaping flattens nested structs depth-first before host ingest.
TEST(ArrowParserTest, FlattensNestedStructsForHostIngest) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const auto status = parseFixture(fixture, "nested.arrows");
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  const auto& stream = fixture.arrow_write_host.streams()[0];
  EXPECT_EQ(
      stream.schema_names,
      (std::vector<std::string>{
          "timestamp_ns", "pose/position/x", "pose/position/y", "pose/position/z", "pose/orientation/x",
          "pose/orientation/y", "pose/orientation/z", "pose/orientation/w", "speed"}));
  EXPECT_EQ(stream.batch_row_counts, (std::vector<int64_t>{3}));
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// Configuration can preserve a top-level struct instead of flattening it.
TEST(ArrowParserTest, PreservesNestedStructWhenFlatteningIsDisabled) {
  ArrowParserFixture fixture;
  fixture.setUp();
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"flatten_structs":false})"));
  const auto status = parseFixture(fixture, "nested.arrows");
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  const auto& stream = fixture.arrow_write_host.streams()[0];
  EXPECT_NE(std::find(stream.schema_names.begin(), stream.schema_names.end(), "pose"), stream.schema_names.end());
  EXPECT_EQ(formatFor(stream, "pose"), "+s");
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// Missing time axes are synthesized from the message timestamp and configured interval.
TEST(ArrowParserTest, SynthesizesTimestampValuesUsingConfiguredInterval) {
  ArrowParserFixture fixture;
  fixture.setUp();
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"synthetic_interval_ns":7})"));
  auto status = parseFixture(fixture, "no_timestamp.arrows", 5000);
  ASSERT_TRUE(status) << status.error();
  ASSERT_TRUE(fixture.handle.loadConfig("{}"));
  status = parseFixture(fixture, "no_timestamp.arrows", 5000);
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 2);
  EXPECT_EQ(fixture.arrow_write_host.streams()[0].timestamp_column, "timestamp_ns");
  EXPECT_EQ(fixture.arrow_write_host.streams()[0].timestamp_values, (std::vector<int64_t>{5000, 5007, 5014}));
  EXPECT_EQ(fixture.arrow_write_host.streams()[1].timestamp_values, (std::vector<int64_t>{5000, 5000, 5000}));
  expectOnlyArrowWrites(fixture.arrow_write_host, 2);
}

/// Typed Arrow timestamps are normalized to int64 nanoseconds before ingest.
TEST(ArrowParserTest, NormalizesTypedTimestampColumnToInt64) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const auto status = parseFixture(fixture, "timestamp_typed.arrows");
  ASSERT_TRUE(status) << status.error();

  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  const auto& stream = fixture.arrow_write_host.streams()[0];
  EXPECT_EQ(stream.timestamp_column, "stamp");
  EXPECT_EQ(formatFor(stream, "stamp"), "l");
  EXPECT_EQ(stream.timestamp_values, (std::vector<int64_t>{1000, 2000, 3000}));
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// A configured timestamp column must exist before a stream reaches the host.
TEST(ArrowParserTest, RejectsMissingConfiguredTimestampColumn) {
  ArrowParserFixture fixture;
  fixture.setUp();
  ASSERT_TRUE(fixture.handle.loadConfig(R"({"timestamp_column":"missing"})"));
  const auto status = parseFixture(fixture, "flat.arrows");

  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("missing"), std::string::npos);
  EXPECT_TRUE(fixture.arrow_write_host.streams().empty());
  expectOnlyArrowWrites(fixture.arrow_write_host, 0);
}

/// Unsupported compression, unsupported IPC view types, and empty input fail during decode.
TEST(ArrowParserTest, RejectsUnsupportedAndEmptyIpcPayloads) {
  ArrowParserFixture fixture;
  fixture.setUp();

  const auto lz4_status = parseFixture(fixture, "flat_lz4.arrows");
  ASSERT_FALSE(lz4_status);
  EXPECT_NE(lz4_status.error().find("lz4"), std::string::npos);

  const auto view_status = parseFixture(fixture, "views.arrows");
  ASSERT_FALSE(view_status);
  EXPECT_NE(view_status.error().find("string_view"), std::string::npos);

  const auto empty_status = fixture.handle.parse(0, PJ::Span<const uint8_t>{});
  ASSERT_FALSE(empty_status);
  EXPECT_NE(empty_status.error().find("empty"), std::string::npos);
  expectOnlyArrowWrites(fixture.arrow_write_host, 0);
}

/// A legacy parser write host reports that the Arrow tail slot is unavailable.
TEST(ArrowParserTest, RejectsHostWithoutArrowStreamSlot) {
  ArrowParserFixture fixture;
  fixture.setUp(false);
  const auto status = parseFixture(fixture, "flat.arrows");

  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("append_arrow_stream"), std::string::npos);
  EXPECT_TRUE(fixture.legacy_write_host.rows().empty());
}

/// Host rejection text is returned and ownership remains with plugin-side RAII.
TEST(ArrowParserTest, PropagatesArrowHostRejection) {
  ArrowParserFixture fixture;
  fixture.setUp();
  fixture.arrow_write_host.failNext();
  const auto status = parseFixture(fixture, "flat.arrows");

  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("configured Arrow stream rejection"), std::string::npos);
  EXPECT_TRUE(fixture.arrow_write_host.streams().empty());
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

/// Dropped-column warnings are deduplicated until the self-described schema changes.
TEST(ArrowParserTest, ReportsDroppedColumnsWhenSchemaSetChanges) {
  ArrowParserFixture fixture;
  fixture.setUp(true, true);

  auto status = parseFixture(fixture, "nested_dropped_scalars.arrows");
  ASSERT_TRUE(status) << status.error();
  ASSERT_EQ(fixture.runtime_host.diagnostics().size(), 1);
  const auto& first_diagnostic = fixture.runtime_host.diagnostics()[0];
  EXPECT_EQ(first_diagnostic.level, PJ::sdk::ParserDiagnosticLevel::Warning);
  EXPECT_EQ(first_diagnostic.stable_code, "parser_arrow.dropped_columns");
  EXPECT_EQ(first_diagnostic.occurrences, 1);
  EXPECT_NE(first_diagnostic.message.find("metadata/date:tdD"), std::string::npos);
  EXPECT_NE(first_diagnostic.message.find("metadata/amount:d:10,2"), std::string::npos);

  status = parseFixture(fixture, "nested_dropped_scalars.arrows");
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(fixture.runtime_host.diagnostics().size(), 1);

  status = parseFixture(fixture, "flat.arrows");
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(fixture.runtime_host.diagnostics().size(), 1);

  status = parseFixture(fixture, "nested_dropped_scalars.arrows");
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(fixture.runtime_host.diagnostics().size(), 2);
  expectOnlyArrowWrites(fixture.arrow_write_host, 4);
}

/// Dropped columns remain non-fatal when the optional runtime service is absent.
TEST(ArrowParserTest, ParsesDroppedColumnsWithoutRuntimeService) {
  ArrowParserFixture fixture;
  fixture.setUp();
  const auto status = parseFixture(fixture, "nested_dropped_scalars.arrows");
  ASSERT_TRUE(status) << status.error();
  ASSERT_EQ(fixture.arrow_write_host.streams().size(), 1);
  expectOnlyArrowWrites(fixture.arrow_write_host, 1);
}

}  // namespace
