#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <data_tamer_v2/data_tamer_parser.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_DATA_TAMER_PARSER_PLUGIN_PATH
#error "PJ_DATA_TAMER_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

struct DTFixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder recorder;

  void setUp() {
    auto lib = PJ::MessageParserLibrary::load(PJ_DATA_TAMER_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library = std::move(*lib);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(recorder.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }

  PJ::Status bindSchema(const std::string& schema_text) {
    const auto* data = reinterpret_cast<const uint8_t*>(schema_text.data());
    return handle.bindSchema("data_tamer", PJ::Span<const uint8_t>(data, schema_text.size()));
  }

  PJ::Status parse(const std::vector<uint8_t>& payload, int64_t ts = 1000) {
    return handle.parse(ts, PJ::Span<const uint8_t>(payload.data(), payload.size()));
  }
};

const PJ::sdk::testing::RecordedField* findField(const PJ::sdk::testing::RecordedRow& row, const std::string& name) {
  for (const auto& f : row.fields) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

// Build a DataTamer binary snapshot payload:
//   uint32_t mask_size | active_mask bytes | uint32_t payload_size | payload bytes
std::vector<uint8_t> buildSnapshot(const DataTamerParser::Schema& schema, const std::vector<double>& values) {
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

// Include the same headers and hash as a writer-produced schema.
std::string schemaText(const std::string& fields) {
  std::string text = "### version: 5\n### channel_name: plugin_test\n\n" + fields;
  text.insert(text.find('\n') + 1, "### hash: " + std::to_string(DataTamerParser::SchemaTextHash(text)) + "\n");
  return text;
}

// ---- Tests ----

TEST(DataTamerParserTest, ManifestContainsEncoding) {
  DTFixture f;
  f.setUp();
  // Manifest uses "encoding" as an array containing all supported encodings
  EXPECT_NE(f.handle.manifest().find("\"data_tamer\""), std::string::npos);
}

TEST(DataTamerParserTest, BasicParsing) {
  DTFixture f;
  f.setUp();

  std::string schema_text = schemaText("float64 temperature\nfloat64 pressure\n");
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuildSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {23.5, 101.3});

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  auto* temp = findField(f.recorder.rows()[0], "/temperature");
  ASSERT_NE(temp, nullptr);
  EXPECT_DOUBLE_EQ(temp->numeric, 23.5);

  auto* pres = findField(f.recorder.rows()[0], "/pressure");
  ASSERT_NE(pres, nullptr);
  EXPECT_DOUBLE_EQ(pres->numeric, 101.3);
}

TEST(DataTamerParserTest, MixedTypes) {
  DTFixture f;
  f.setUp();

  std::string schema_text = schemaText("int32 count\nfloat32 ratio\nfloat64 value\n");
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuildSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {42.0, 0.5, 3.14});

  ASSERT_TRUE(f.parse(payload));
  ASSERT_EQ(f.recorder.rows().size(), 1u);

  auto* count = findField(f.recorder.rows()[0], "/count");
  ASSERT_NE(count, nullptr);
  EXPECT_DOUBLE_EQ(count->numeric, 42.0);

  auto* ratio = findField(f.recorder.rows()[0], "/ratio");
  ASSERT_NE(ratio, nullptr);
  EXPECT_NEAR(ratio->numeric, 0.5, 1e-6);

  auto* val = findField(f.recorder.rows()[0], "/value");
  ASSERT_NE(val, nullptr);
  EXPECT_DOUBLE_EQ(val->numeric, 3.14);
}

TEST(DataTamerParserTest, TimestampPreserved) {
  DTFixture f;
  f.setUp();

  std::string schema_text = schemaText("float64 x\n");
  ASSERT_TRUE(f.bindSchema(schema_text));

  auto schema = DataTamerParser::BuildSchemaFromText(schema_text);
  auto payload = buildSnapshot(schema, {1.0});

  ASSERT_TRUE(f.parse(payload, 99999));
  EXPECT_EQ(f.recorder.rows()[0].timestamp, 99999);
}

TEST(DataTamerParserTest, EmptyPayload) {
  DTFixture f;
  f.setUp();

  std::string schema_text = schemaText("float64 x\n");
  ASSERT_TRUE(f.bindSchema(schema_text));

  // All fields inactive: mask = all 0s.
  std::vector<uint8_t> payload;
  auto push32 = [&](uint32_t val) {
    auto* p = reinterpret_cast<const uint8_t*>(&val);
    payload.insert(payload.end(), p, p + 4);
  };
  push32(1);             // mask_size = 1
  payload.push_back(0);  // mask = 0 (field inactive)
  push32(0);             // payload_size = 0

  ASSERT_TRUE(f.parse(payload));
  // No fields emitted since the field is inactive.
  EXPECT_EQ(f.recorder.rows().size(), 0u);
}

// A mask_size larger than the remaining buffer used to underflow BufferSpan::size
// and walk the read cursor off the end into a wild pointer (segfault on the next
// Deserialize). It must now be rejected cleanly. Bare 0xFFFFFFFF is the minimal
// reproducer: mask_size = 4 GiB with only the 4 header bytes present.
TEST(DataTamerParserTest, OversizedMaskSizeIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\n")));

  std::vector<uint8_t> payload = {0xFF, 0xFF, 0xFF, 0xFF};  // mask_size = 0xFFFFFFFF
  EXPECT_FALSE(f.parse(payload));
  EXPECT_EQ(f.recorder.rows().size(), 0u);
}

// A payload shorter than the 4-byte mask_size header must be rejected before the
// read (Deserialize<> memcpy's before it bounds-checks).
TEST(DataTamerParserTest, TruncatedBeforeMaskSizeIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\n")));

  std::vector<uint8_t> payload = {0x00, 0x01};  // only 2 of the 4 header bytes
  EXPECT_FALSE(f.parse(payload));
  EXPECT_EQ(f.recorder.rows().size(), 0u);
}

// A payload_size larger than the bytes left after the mask must be rejected.
TEST(DataTamerParserTest, OversizedPayloadSizeIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\n")));

  std::vector<uint8_t> payload;
  auto push32 = [&](uint32_t val) {
    auto* p = reinterpret_cast<const uint8_t*>(&val);
    payload.insert(payload.end(), p, p + 4);
  };
  push32(1);                // mask_size = 1
  payload.push_back(0xFF);  // mask byte
  push32(0xFFFFFFFF);       // payload_size = 4 GiB, nothing follows
  EXPECT_FALSE(f.parse(payload));
  EXPECT_EQ(f.recorder.rows().size(), 0u);
}

const std::filesystem::path kVectorsDir = PJ_DATA_TAMER_VECTORS_DIR;

std::string readVectorText(const std::string& filename) {
  std::ifstream input(kVectorsDir / filename, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open DataTamer vector: " + filename);
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> readVectorBytes(const std::string& filename) {
  const auto text = readVectorText(filename);
  return {text.begin(), text.end()};
}

template <typename T>
void appendWire(std::vector<uint8_t>& bytes, T value) {
  const auto* data = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), data, data + sizeof(value));
}

std::vector<uint8_t> frameMessage(const std::vector<uint8_t>& mask, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> message;
  appendWire(message, static_cast<uint32_t>(mask.size()));
  message.insert(message.end(), mask.begin(), mask.end());
  appendWire(message, static_cast<uint32_t>(payload.size()));
  message.insert(message.end(), payload.begin(), payload.end());
  return message;
}

// Check the functional ABI too: ParserWriteRecorder converts int64/uint64 to double,
// which cannot detect lost low bits in the golden uint64 value.
void expectScalarValues(PJ::Span<const PJ_named_field_value_t> fields, nlohmann::json expected) {
  ASSERT_EQ(fields.size(), expected.size());
  for (const auto& field : fields) {
    const std::string name(field.name.data, field.name.size);
    SCOPED_TRACE(name);
    ASSERT_FALSE(name.empty());
    ASSERT_EQ(name.front(), '/');
    const auto key = name.substr(1);
    ASSERT_TRUE(expected.contains(key));
    auto value = expected.at(key);
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      ASSERT_EQ(text.size(), 1u);
      value = static_cast<uint8_t>(text.front());
    }
    EXPECT_FALSE(field.is_null);
    const auto& scalar = field.value;
    switch (scalar.type) {
      case PJ_PRIMITIVE_TYPE_BOOL:
        EXPECT_EQ(scalar.data.as_bool != 0, value.get<bool>());
        break;
      case PJ_PRIMITIVE_TYPE_INT8:
        EXPECT_EQ(scalar.data.as_int8, value.get<int8_t>());
        break;
      case PJ_PRIMITIVE_TYPE_UINT8:
        EXPECT_EQ(scalar.data.as_uint8, value.get<uint8_t>());
        break;
      case PJ_PRIMITIVE_TYPE_INT16:
        EXPECT_EQ(scalar.data.as_int16, value.get<int16_t>());
        break;
      case PJ_PRIMITIVE_TYPE_UINT16:
        EXPECT_EQ(scalar.data.as_uint16, value.get<uint16_t>());
        break;
      case PJ_PRIMITIVE_TYPE_INT32:
        EXPECT_EQ(scalar.data.as_int32, value.get<int32_t>());
        break;
      case PJ_PRIMITIVE_TYPE_UINT32:
        EXPECT_EQ(scalar.data.as_uint32, value.get<uint32_t>());
        break;
      case PJ_PRIMITIVE_TYPE_INT64:
        EXPECT_EQ(scalar.data.as_int64, value.get<int64_t>());
        break;
      case PJ_PRIMITIVE_TYPE_UINT64:
        EXPECT_EQ(scalar.data.as_uint64, value.get<uint64_t>());
        break;
      case PJ_PRIMITIVE_TYPE_FLOAT32:
        EXPECT_EQ(std::bit_cast<uint32_t>(scalar.data.as_float32), std::bit_cast<uint32_t>(value.get<float>()));
        break;
      case PJ_PRIMITIVE_TYPE_FLOAT64:
        EXPECT_EQ(std::bit_cast<uint64_t>(scalar.data.as_float64), std::bit_cast<uint64_t>(value.get<double>()));
        break;
      default:
        FAIL() << "Unexpected scalar type " << scalar.type;
    }
    expected.erase(key);
  }
  EXPECT_TRUE(expected.empty());
}

void expectFunctionalSnapshot(DTFixture& fixture, const std::vector<uint8_t>& message, const nlohmann::json& expected) {
  size_t calls = 0;
  auto status = fixture.handle.parseScalarsFunctional(
      987654321, PJ::Span<const uint8_t>(message.data(), message.size()),
      [&](std::optional<PJ::Timestamp> timestamp, PJ::Span<const PJ_named_field_value_t> fields) {
        ++calls;
        EXPECT_FALSE(timestamp.has_value());  // The host supplies the receive/log time.
        expectScalarValues(fields, expected);
        return PJ::okStatus();
      });
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(calls, 1u);
}

void expectRejected(DTFixture& fixture, const std::vector<uint8_t>& message, const std::string& error) {
  auto status = fixture.parse(message);
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find(error), std::string::npos) << status.error();
  EXPECT_TRUE(fixture.recorder.rows().empty());
  size_t calls = 0;
  status = fixture.handle.parseScalarsFunctional(
      1000, PJ::Span<const uint8_t>(message.data(), message.size()),
      [&](std::optional<PJ::Timestamp>, PJ::Span<const PJ_named_field_value_t>) {
        ++calls;
        return PJ::okStatus();
      });
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find(error), std::string::npos) << status.error();
  EXPECT_EQ(calls, 0u);
}

class DataTamerGoldenTest : public testing::TestWithParam<const char*> {};

TEST_P(DataTamerGoldenTest, FullAndMaskedSnapshots) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("schema." + std::string(GetParam()))));
  const auto oracle = nlohmann::json::parse(readVectorText("expected.json"));
  const std::map<std::string, PJ::PrimitiveType> types = {
      {"flag", PJ::PrimitiveType::kBool},    {"letter", PJ::PrimitiveType::kUint8},
      {"i8", PJ::PrimitiveType::kInt8},      {"u8", PJ::PrimitiveType::kUint8},
      {"i16", PJ::PrimitiveType::kInt16},    {"u16", PJ::PrimitiveType::kUint16},
      {"i32", PJ::PrimitiveType::kInt32},    {"u32", PJ::PrimitiveType::kUint32},
      {"i64", PJ::PrimitiveType::kInt64},    {"u64", PJ::PrimitiveType::kUint64},
      {"f32", PJ::PrimitiveType::kFloat32},  {"f64", PJ::PrimitiveType::kFloat64},
      {"vec", PJ::PrimitiveType::kFloat64},  {"arr", PJ::PrimitiveType::kInt32},
      {"pose", PJ::PrimitiveType::kFloat64}, {"points", PJ::PrimitiveType::kFloat64}};

  for (const std::string sample : {"full", "masked"}) {
    SCOPED_TRACE(sample);
    const auto message = readVectorBytes("snapshot_" + sample + ".mcap_message");
    const auto& expected = oracle.at(sample);
    const auto row_count = f.recorder.rows().size();
    ASSERT_TRUE(f.parse(message, 987654321));
    ASSERT_EQ(f.recorder.rows().size(), row_count + 1);
    const auto& row = f.recorder.rows().back();
    EXPECT_EQ(row.timestamp, 987654321);
    ASSERT_EQ(row.fields.size(), expected.size());
    for (const auto& [name, value] : expected.items()) {
      const auto* field = findField(row, "/" + name);
      ASSERT_NE(field, nullptr) << name;
      const auto base = name.substr(0, name.find_first_of("[/"));
      const auto type = name == "pose/stamp" ? PJ::PrimitiveType::kUint32 : types.at(base);
      EXPECT_EQ(field->type, type) << name;
      if (value.is_boolean()) {
        EXPECT_EQ(field->bool_value, value.get<bool>());
      } else if (value.is_string()) {
        EXPECT_EQ(field->numeric, static_cast<uint8_t>(value.get<std::string>().front()));
      } else {
        EXPECT_DOUBLE_EQ(field->numeric, value.get<double>()) << name;
      }
    }
    expectFunctionalSnapshot(f, message, expected);
  }
}

TEST_P(DataTamerGoldenTest, NestedFullAndMaskedSnapshots) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("schema_nested." + std::string(GetParam()))));
  // Upstream supplies the nested schemas but no nested snapshot/oracle files.
  const auto oracle = nlohmann::json::parse(R"({
    "arm/joint_1/position": 1.0, "arm/joint_1/velocity": 2.0,
    "arm/joint_2/position": 3.0, "arm/joint_2/velocity": 4.0,
    "arm/mode": 5, "state": 6, "arm/late": 7.0, "single/x": 8.0,
    "dup": 9, "dup/a": 10.0, "dup/b": 11.0, "on": 12, "7up/x": 13.0, "7up/y": 14.0,
    "cart/pose/position/x": 1.0, "cart/pose/position/y": 2.0,
    "cart/pose/position/z": 3.0, "cart/pose/stamp": 42
  })");
  const std::vector<DataTamerParser::VarNumber> values = {1.0, 2.0,        3.0,  4.0,  uint8_t{5},  uint8_t{6}, 7.0,
                                                          8.0, uint8_t{9}, 10.0, 11.0, uint8_t{12}, 13.0,       14.0};
  for (bool masked : {false, true}) {
    SCOPED_TRACE(masked);
    auto expected = oracle;
    std::vector<uint8_t> data;
    for (size_t i = 0; i < values.size(); ++i) {
      if (!masked || i != 1) {
        std::visit([&](auto value) { appendWire(data, value); }, values[i]);
      }
    }
    if (masked) {
      expected.erase("arm/joint_1/velocity");
      for (const auto* key :
           {"cart/pose/position/x", "cart/pose/position/y", "cart/pose/position/z", "cart/pose/stamp"}) {
        expected.erase(key);
      }
    } else {
      for (double value : {1.0, 2.0, 3.0}) {
        appendWire(data, value);
      }
      appendWire(data, uint32_t{42});
    }
    const auto message =
        frameMessage(masked ? std::vector<uint8_t>{0xFD, 0x3F} : std::vector<uint8_t>{0xFF, 0x7F}, data);
    ASSERT_TRUE(f.parse(message));
    ASSERT_EQ(f.recorder.rows().back().fields.size(), expected.size());
    for (const auto& [name, value] : expected.items()) {
      const auto* field = findField(f.recorder.rows().back(), "/" + name);
      ASSERT_NE(field, nullptr) << name;
      EXPECT_DOUBLE_EQ(field->numeric, value.get<double>()) << name;
    }
    expectFunctionalSnapshot(f, message, expected);
  }
}

INSTANTIATE_TEST_SUITE_P(SchemaFormats, DataTamerGoldenTest, testing::Values("txt", "yaml"), [](const auto& case_info) {
  return std::string(case_info.param) == "txt" ? "LineV5" : "YamlV6";
});

TEST(DataTamerParserTest, VersionlessSchemaIsRejectedWithClearError) {
  DTFixture f;
  f.setUp();
  const auto status = f.bindSchema("float64 x\n");
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("### version:"), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("0.7.0"), std::string::npos) << status.error();
}

TEST(DataTamerParserTest, BoolAndCharBytesPreserveTypes) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("bool off\nbool on\nchar byte\n")));
  const auto message = frameMessage({7}, {0, 2, 0xFF});
  ASSERT_TRUE(f.parse(message));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  const auto& fields = f.recorder.rows()[0].fields;
  ASSERT_EQ(fields.size(), 3u);
  EXPECT_EQ(fields[0].type, PJ::PrimitiveType::kBool);
  EXPECT_FALSE(fields[0].bool_value);
  EXPECT_EQ(fields[1].type, PJ::PrimitiveType::kBool);
  EXPECT_TRUE(fields[1].bool_value);
  EXPECT_EQ(fields[2].type, PJ::PrimitiveType::kUint8);
  EXPECT_EQ(fields[2].numeric, 255);
  expectFunctionalSnapshot(f, message, {{"off", false}, {"on", true}, {"byte", 255}});
}

TEST(DataTamerParserTest, TruncatedBeforePayloadSizeIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\n")));
  expectRejected(f, {1, 0, 0, 0, 1}, "truncated");
}

TEST(DataTamerParserTest, TrailingMcapBytesIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("schema.txt")));
  auto message = readVectorBytes("snapshot_full.mcap_message");
  message.push_back(0);
  expectRejected(f, message, "trailing bytes");
}

TEST(DataTamerParserTest, TrailingSnapshotPayloadIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\n")));
  expectRejected(f, frameMessage({1}, std::vector<uint8_t>(9, 0)), "does not match");
  // A failure after decoding x must not leak a partial record into the next parse.
  ASSERT_TRUE(f.parse(frameMessage({1}, std::vector<uint8_t>(8, 0))));
  ASSERT_EQ(f.recorder.rows().size(), 1u);
  EXPECT_EQ(f.recorder.rows()[0].fields.size(), 1u);
}

TEST(DataTamerParserTest, TruncatedFieldIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64 x\nfloat64 y\n")));
  expectRejected(f, frameMessage({3}, std::vector<uint8_t>(15, 0)), "truncated");
}

TEST(DataTamerParserTest, ShortActiveMaskIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("schema.txt")));
  expectRejected(f, frameMessage({0}, {}), "active mask shorter");
}

TEST(DataTamerParserTest, OversizedVectorLengthIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(schemaText("float64[] values\n")));
  expectRejected(f, frameMessage({1}, {0xFF, 0xFF, 0xFF, 0xFF}), "truncated");
}

TEST(DataTamerParserTest, Version4SchemasBindAndDecode) {
  for (const std::string name : {"features", "second", "opaque"}) {
    SCOPED_TRACE(name);
    DTFixture f;
    f.setUp();
    const auto text = readVectorText("v4/schema_" + name + ".txt");
    ASSERT_TRUE(f.bindSchema(text));
    const auto schema = DataTamerParser::BuildSchemaFromText(text);
    std::vector<uint8_t> mask((schema.fields.size() + 7) / 8, 0);
    ASSERT_TRUE(f.parse(frameMessage(mask, {})));  // Includes disabled opaque fields.
    EXPECT_TRUE(f.recorder.rows().empty());
    mask[0] = 1;
    std::vector<uint8_t> data;
    nlohmann::json expected;
    if (name == "features") {
      appendWire(data, uint8_t{1});
      expected = {{"b", true}};
    } else if (name == "second") {
      appendWire(data, 1.25);
      expected = {{"x", 1.25}};
    } else {
      appendWire(data, int32_t{-42});
      expected = {{"ocount", -42}};
    }
    const auto message = frameMessage(mask, data);
    ASSERT_TRUE(f.parse(message));
    ASSERT_EQ(f.recorder.rows().size(), 1u);
    expectFunctionalSnapshot(f, message, expected);
  }
}

TEST(DataTamerParserTest, Version4NestedTypesDeclaredOutOfOrder) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("v4/schema_features.txt")));
  std::vector<uint8_t> data;
  for (double value : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 1.0, 0.0, 0.0, 0.0}) {
    appendWire(data, value);
  }
  appendWire(data, uint32_t{42});
  const auto message = frameMessage({0, 0, 0x0C}, data);  // point and pose only
  ASSERT_TRUE(f.parse(message));
  expectFunctionalSnapshot(
      f, message,
      {{"point/x", 1.0},
       {"point/y", 2.0},
       {"point/z", 3.0},
       {"pose/position/x", 4.0},
       {"pose/position/y", 5.0},
       {"pose/position/z", 6.0},
       {"pose/orientation/w", 1.0},
       {"pose/orientation/x", 0.0},
       {"pose/orientation/y", 0.0},
       {"pose/orientation/z", 0.0},
       {"pose/stamp", 42}});
}

TEST(DataTamerParserTest, Version4OpaqueFieldIsRejected) {
  DTFixture f;
  f.setUp();
  ASSERT_TRUE(f.bindSchema(readVectorText("v4/schema_opaque.txt")));
  expectRejected(f, frameMessage({3}, std::vector<uint8_t>(12, 0)), "opaque encoding");
}

}  // namespace
