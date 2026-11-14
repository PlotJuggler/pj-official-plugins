#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>
#include <vector>

#include "../ulog_container.hpp"
#include "../ulog_flatten.hpp"

namespace {

// Helper to build a binary ULog stream in memory.
class ULogBuilder {
 public:
  void writeHeader(uint64_t timestamp_us = 1000000) {
    buf_.push_back('U');
    buf_.push_back('L');
    buf_.push_back('o');
    buf_.push_back('g');
    buf_.push_back(0x01);
    buf_.push_back(0x12);
    buf_.push_back(0x35);
    buf_.push_back(0x00);
    appendLe(timestamp_us);
  }

  void writeFlagBits() {
    std::vector<uint8_t> payload(40, 0);
    writeMessage('B', payload);
  }

  void writeFormat(const std::string& format_str) {
    // ULog format strings must end with a trailing semicolon for ulog_cpp to parse correctly.
    std::string fmt = format_str;
    if (!fmt.empty() && fmt.back() != ';') {
      fmt.push_back(';');
    }
    std::vector<uint8_t> payload(fmt.begin(), fmt.end());
    writeMessage('F', payload);
  }

  void writeParameterInt(const std::string& name, int32_t value) {
    std::string type_and_name = "int32_t " + name;
    std::vector<uint8_t> payload;
    auto key_len = static_cast<uint8_t>(type_and_name.size());
    payload.push_back(key_len);
    payload.insert(payload.end(), type_and_name.begin(), type_and_name.end());
    appendLe(payload, value);
    writeMessage('P', payload);
  }

  void writeParameterFloat(const std::string& name, float value) {
    std::string type_and_name = "float " + name;
    std::vector<uint8_t> payload;
    auto key_len = static_cast<uint8_t>(type_and_name.size());
    payload.push_back(key_len);
    payload.insert(payload.end(), type_and_name.begin(), type_and_name.end());
    appendLe(payload, value);
    writeMessage('P', payload);
  }

  void writeSubscription(uint16_t msg_id, uint8_t multi_id, const std::string& format_name) {
    std::vector<uint8_t> payload;
    payload.push_back(multi_id);
    appendLe(payload, msg_id);
    payload.insert(payload.end(), format_name.begin(), format_name.end());
    writeMessage('A', payload);
  }

  void writeData(uint16_t msg_id, const std::vector<uint8_t>& field_data) {
    std::vector<uint8_t> payload;
    appendLe(payload, msg_id);
    payload.insert(payload.end(), field_data.begin(), field_data.end());
    writeMessage('D', payload);
  }

  // Logged string ('L'): level '0'..'7', timestamp, text.
  void writeLogging(char level, uint64_t timestamp_us, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(level));
    appendLe(payload, timestamp_us);
    payload.insert(payload.end(), text.begin(), text.end());
    writeMessage('L', payload);
  }

  // Tagged logged string ('C'): level, tag, timestamp, text.
  void writeTaggedLogging(char level, uint16_t tag, uint64_t timestamp_us, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(level));
    appendLe(payload, tag);
    appendLe(payload, timestamp_us);
    payload.insert(payload.end(), text.begin(), text.end());
    writeMessage('C', payload);
  }

  const std::vector<uint8_t>& data() const {
    return buf_;
  }

 private:
  std::vector<uint8_t> buf_;

  void writeMessage(char msg_type, const std::vector<uint8_t>& payload) {
    auto msg_size = static_cast<uint16_t>(payload.size());
    appendLe(msg_size);
    buf_.push_back(static_cast<uint8_t>(msg_type));
    buf_.insert(buf_.end(), payload.begin(), payload.end());
  }

  template <typename T>
  void appendLe(T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf_.insert(buf_.end(), bytes, bytes + sizeof(T));
  }

  template <typename T>
  static void appendLe(std::vector<uint8_t>& vec, T value) {
    // Byte by byte: gcc 15 at -O3 flags a range insert into an empty vector (-Wnonnull).
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) {
      vec.push_back(bytes[i]);
    }
  }
};

// Helper to build field data for a data message.
class FieldDataBuilder {
 public:
  template <typename T>
  void append(T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) {
      data_.push_back(bytes[i]);  // see ULogBuilder::appendLe
    }
  }

  std::vector<uint8_t> build() const {
    return data_;
  }

 private:
  std::vector<uint8_t> data_;
};

// Parse a ULogBuilder into a DataContainer.
std::shared_ptr<ulog_cpp::DataContainer> parseBuilder(const ULogBuilder& builder) {
  auto container = std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  return container;
}

TEST(ULogSourceTest, InvalidDataReportsFatalError) {
  std::vector<uint8_t> bad_data = {'N', 'O', 'T', '_', 'U', 'L', 'O', 'G', 0, 0, 0, 0, 0, 0, 0, 0};
  auto container = std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
  ulog_cpp::Reader reader{container};
  reader.readChunk(bad_data.data(), static_cast<int>(bad_data.size()));
  EXPECT_TRUE(container->hadFatalError());
}

TEST(ULogSourceTest, HeaderTimestamp) {
  ULogBuilder builder;
  builder.writeHeader(5000000);
  builder.writeFlagBits();
  builder.writeFormat("empty_msg:uint64_t timestamp");
  builder.writeSubscription(1, 0, "empty_msg");

  auto container = parseBuilder(builder);
  EXPECT_FALSE(container->hadFatalError());
  EXPECT_EQ(container->fileHeader().header().timestamp, 5000000u);
}

TEST(ULogSourceTest, DataMessageExtraction) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("sensor_accel:uint64_t timestamp;float x;float y;float z");
  builder.writeSubscription(1, 0, "sensor_accel");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(2000000);
    fields.append<float>(1.5f);
    fields.append<float>(2.5f);
    fields.append<float>(9.8f);
    builder.writeData(1, fields.build());
  }
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(3000000);
    fields.append<float>(1.6f);
    fields.append<float>(2.6f);
    fields.append<float>(9.7f);
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  auto sub = container->subscription("sensor_accel");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 2u);

  // Check first sample.
  auto sample0 = sub->at(0);
  EXPECT_EQ(sample0["timestamp"].as<uint64_t>(), 2000000u);
  EXPECT_FLOAT_EQ(sample0["x"].as<float>(), 1.5f);
  EXPECT_FLOAT_EQ(sample0["y"].as<float>(), 2.5f);
  EXPECT_FLOAT_EQ(sample0["z"].as<float>(), 9.8f);

  // Check second sample.
  auto sample1 = sub->at(1);
  EXPECT_EQ(sample1["timestamp"].as<uint64_t>(), 3000000u);
  EXPECT_FLOAT_EQ(sample1["x"].as<float>(), 1.6f);
}

TEST(ULogSourceTest, MultiIdSupport) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("sensor_gyro:uint64_t timestamp;float x");
  builder.writeSubscription(1, 0, "sensor_gyro");
  builder.writeSubscription(2, 1, "sensor_gyro");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<float>(10.0f);
    builder.writeData(1, fields.build());
  }
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<float>(20.0f);
    builder.writeData(2, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  // Both subscriptions should exist.
  auto sub0 = container->subscription("sensor_gyro", 0);
  auto sub1 = container->subscription("sensor_gyro", 1);
  ASSERT_NE(sub0, nullptr);
  ASSERT_NE(sub1, nullptr);
  ASSERT_EQ(sub0->size(), 1u);
  ASSERT_EQ(sub1->size(), 1u);

  EXPECT_FLOAT_EQ(sub0->at(0)["x"].as<float>(), 10.0f);
  EXPECT_FLOAT_EQ(sub1->at(0)["x"].as<float>(), 20.0f);
}

TEST(ULogSourceTest, ArrayFieldAccess) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("with_array:uint64_t timestamp;float[3] values");
  builder.writeSubscription(1, 0, "with_array");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<float>(1.0f);
    fields.append<float>(2.0f);
    fields.append<float>(3.0f);
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  auto sub = container->subscription("with_array");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 1u);

  auto sample = sub->at(0);
  // Array elements accessed via index operator on the value.
  EXPECT_FLOAT_EQ(sample["values"][0].as<float>(), 1.0f);
  EXPECT_FLOAT_EQ(sample["values"][1].as<float>(), 2.0f);
  EXPECT_FLOAT_EQ(sample["values"][2].as<float>(), 3.0f);
}

TEST(ULogSourceTest, ParameterParsing) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeParameterInt("SYS_AUTOSTART", 4001);
  builder.writeParameterFloat("MC_ROLLRATE_P", 0.15f);
  builder.writeSubscription(1, 0, "dummy");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  const auto& params = container->initialParameters();
  ASSERT_TRUE(params.count("SYS_AUTOSTART") > 0);
  EXPECT_EQ(params.at("SYS_AUTOSTART").value().as<int32_t>(), 4001);

  ASSERT_TRUE(params.count("MC_ROLLRATE_P") > 0);
  EXPECT_FLOAT_EQ(params.at("MC_ROLLRATE_P").value().as<float>(), 0.15f);
}

TEST(ULogSourceTest, NestedFormatAccess) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("vec3:float x;float y;float z");
  builder.writeFormat("pose:uint64_t timestamp;vec3 position;float heading");
  builder.writeSubscription(1, 0, "pose");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<float>(1.0f);   // position.x
    fields.append<float>(2.0f);   // position.y
    fields.append<float>(3.0f);   // position.z
    fields.append<float>(45.0f);  // heading
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  auto sub = container->subscription("pose");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 1u);

  auto sample = sub->at(0);
  EXPECT_FLOAT_EQ(sample["position"]["x"].as<float>(), 1.0f);
  EXPECT_FLOAT_EQ(sample["position"]["y"].as<float>(), 2.0f);
  EXPECT_FLOAT_EQ(sample["position"]["z"].as<float>(), 3.0f);
  EXPECT_FLOAT_EQ(sample["heading"].as<float>(), 45.0f);
}

TEST(ULogSourceTest, AllNumericTypes) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat(
      "all_types:uint64_t timestamp;uint8_t u8;int8_t i8;uint16_t u16;int16_t i16;"
      "uint32_t u32;int32_t i32;uint64_t u64;int64_t i64;"
      "float f32;double f64;bool flag");
  builder.writeSubscription(1, 0, "all_types");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<uint8_t>(255);
    fields.append<int8_t>(-1);
    fields.append<uint16_t>(65535);
    fields.append<int16_t>(-100);
    fields.append<uint32_t>(100000);
    fields.append<int32_t>(-200000);
    fields.append<uint64_t>(99999999);
    fields.append<int64_t>(-99999999);
    fields.append<float>(3.14f);
    fields.append<double>(2.71828);
    fields.append<uint8_t>(1);
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());

  auto sub = container->subscription("all_types");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 1u);

  auto sample = sub->at(0);
  EXPECT_EQ(sample["u8"].as<uint8_t>(), 255);
  EXPECT_EQ(sample["i8"].as<int8_t>(), -1);
  EXPECT_EQ(sample["u16"].as<uint16_t>(), 65535);
  EXPECT_EQ(sample["i16"].as<int16_t>(), -100);
  EXPECT_EQ(sample["u32"].as<uint32_t>(), 100000u);
  EXPECT_EQ(sample["i32"].as<int32_t>(), -200000);
  EXPECT_EQ(sample["u64"].as<uint64_t>(), 99999999u);
  EXPECT_EQ(sample["i64"].as<int64_t>(), -99999999);
  EXPECT_FLOAT_EQ(sample["f32"].as<float>(), 3.14f);
  EXPECT_DOUBLE_EQ(sample["f64"].as<double>(), 2.71828);
  EXPECT_EQ(sample["flag"].as<bool>(), true);
}

// --- Tests for the plugin's own flatten/extract path (ulog_flatten.hpp) ---
//
// The tests above validate ulog_cpp's typed accessors. The importer does NOT use
// those at runtime: it flattens the format and reads raw bytes at resolved
// offsets. These tests drive that production path directly.

// Decode a scalar leaf from raw bytes the same way the plugin does, returning a
// double for comparison against known inputs.
double decodeLeaf(const std::vector<uint8_t>& raw, size_t offset, ulog_cpp::Field::BasicType type) {
  const uint8_t* p = raw.data() + offset;
  switch (type) {
    case ulog_cpp::Field::BasicType::INT8:
      return static_cast<double>(*reinterpret_cast<const int8_t*>(p));
    case ulog_cpp::Field::BasicType::UINT8:
    case ulog_cpp::Field::BasicType::CHAR:
      return static_cast<double>(*p);
    case ulog_cpp::Field::BasicType::BOOL:
      return static_cast<double>(*p != 0);
    case ulog_cpp::Field::BasicType::INT16: {
      int16_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::UINT16: {
      uint16_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::INT32: {
      int32_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::UINT32: {
      uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::INT64: {
      int64_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::UINT64: {
      uint64_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::FLOAT: {
      float v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::DOUBLE: {
      double v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    default:
      return 0.0;
  }
}

// Build a message that exercises nesting + arrays + a scalar tail, then verify the
// flattened names and the values decoded at the offsets forEachFlatLeaf reports.
// This locks in the nested base-offset accumulation the importer relies on.
TEST(ULogFlattenTest, NamesAndOffsetsMatchKnownRecord) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("vec3:float x;float y;float z");
  builder.writeFormat("sample:uint64_t timestamp;vec3 pos;uint8_t mode;float[2] arr;int32_t code");
  builder.writeSubscription(1, 0, "sample");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(7000000);  // timestamp (skipped)
    fields.append<float>(1.0f);        // pos.x
    fields.append<float>(2.0f);        // pos.y
    fields.append<float>(3.0f);        // pos.z
    fields.append<uint8_t>(42);        // mode
    fields.append<float>(4.0f);        // arr.00
    fields.append<float>(5.0f);        // arr.01
    fields.append<int32_t>(-77);       // code
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("sample");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 1u);

  // Names, in flattened order.
  std::vector<std::string> names;
  ulog_flatten::collectFlatFieldNames(*sub->format(), {}, names);
  const std::vector<std::string> expected_names = {"pos.x", "pos.y", "pos.z", "mode", "arr.00", "arr.01", "code"};
  EXPECT_EQ(names, expected_names);

  // Values decoded at the offsets forEachFlatLeaf reports, in the same order.
  const auto& raw = sub->rawSamples()[0].data();
  std::vector<double> values;
  size_t max_end = 0;
  ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) {
    ASSERT_LE(leaf.offset + leaf.size, raw.size());  // offsets stay inside the record
    EXPECT_FALSE(leaf.is_string);
    max_end = std::max(max_end, leaf.offset + leaf.size);
    values.push_back(decodeLeaf(raw, leaf.offset, leaf.type));
  });

  const std::vector<double> expected_values = {1.0, 2.0, 3.0, 42.0, 4.0, 5.0, -77.0};
  ASSERT_EQ(values.size(), expected_values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    EXPECT_DOUBLE_EQ(values[i], expected_values[i]) << "leaf " << i << " (" << names[i] << ")";
  }

  // The furthest byte any leaf reads must equal the record size, so a caller can
  // bound-check with `offset + size <= raw_size` and a truncated record trips it.
  EXPECT_EQ(max_end, raw.size());
  EXPECT_EQ(max_end, static_cast<size_t>(sub->format()->sizeBytes()));
}

// Names and leaf count must stay in lockstep so the importer can zip them.
TEST(ULogFlattenTest, NameAndLeafCountsAgree) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("vec3:float x;float y;float z");
  builder.writeFormat("sample:uint64_t timestamp;vec3 pos;float[2] arr;int32_t code");
  builder.writeSubscription(1, 0, "sample");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("sample");
  ASSERT_NE(sub, nullptr);

  std::vector<std::string> names;
  ulog_flatten::collectFlatFieldNames(*sub->format(), {}, names);

  size_t leaf_count = 0;
  ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf&) { ++leaf_count; });

  EXPECT_EQ(names.size(), leaf_count);
}

// --- Timestamp field lookup ---
//
// The ULog spec requires every subscribed format to carry a `uint64_t timestamp`
// field but does NOT require it to be the first field. The importer must locate
// it by name rather than assume byte offset 0.

TEST(ULogFlattenTest, TimestampOffsetIsFoundByNameNotPosition) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("odd:uint8_t mode;uint64_t timestamp;float x");
  builder.writeSubscription(1, 0, "odd");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("odd");

  auto offset = ulog_flatten::findTimestampOffset(*sub->format());
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 1u);

  // The timestamp is still excluded from the flattened series.
  std::vector<std::string> names;
  ulog_flatten::collectFlatFieldNames(*sub->format(), {}, names);
  EXPECT_EQ(names, (std::vector<std::string>{"mode", "x"}));
}

TEST(ULogFlattenTest, MissingTimestampFieldReportsNullopt) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("no_ts:uint32_t counter;float x");
  builder.writeSubscription(1, 0, "no_ts");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("no_ts");

  EXPECT_FALSE(ulog_flatten::findTimestampOffset(*sub->format()).has_value());
}

TEST(ULogFlattenTest, TimestampFieldOfWrongTypeIsIgnored) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("bad_ts:uint32_t timestamp;float x");
  builder.writeSubscription(1, 0, "bad_ts");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("bad_ts");

  EXPECT_FALSE(ulog_flatten::findTimestampOffset(*sub->format()).has_value());
}

// --- char[N] as strings (PlotJuggler#1387) ---
//
// A `char[N]` field is a fixed-width string per the ULog spec, not N numeric
// samples. It flattens to ONE leaf whose size spans the whole array.

TEST(ULogFlattenTest, CharArrayFlattensToSingleStringLeaf) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("named:uint64_t timestamp;char[8] name;float v");
  builder.writeSubscription(1, 0, "named");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("named");

  std::vector<std::string> names;
  ulog_flatten::collectFlatFieldNames(*sub->format(), {}, names);
  EXPECT_EQ(names, (std::vector<std::string>{"name", "v"}));

  std::vector<ulog_flatten::FlatLeaf> leaves;
  ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) { leaves.push_back(leaf); });
  ASSERT_EQ(leaves.size(), 2u);

  EXPECT_EQ(leaves[0].offset, 8u);
  EXPECT_EQ(leaves[0].type, ulog_cpp::Field::BasicType::CHAR);
  EXPECT_EQ(leaves[0].size, 8u);
  EXPECT_TRUE(leaves[0].is_string);

  EXPECT_EQ(leaves[1].offset, 16u);
  EXPECT_EQ(leaves[1].type, ulog_cpp::Field::BasicType::FLOAT);
  EXPECT_EQ(leaves[1].size, 4u);
  EXPECT_FALSE(leaves[1].is_string);
}

TEST(ULogFlattenTest, ScalarCharStaysNumericLeaf) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("one_char:uint64_t timestamp;char c");
  builder.writeSubscription(1, 0, "one_char");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("one_char");

  std::vector<ulog_flatten::FlatLeaf> leaves;
  ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) { leaves.push_back(leaf); });
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0].size, 1u);
  EXPECT_FALSE(leaves[0].is_string);
}

TEST(ULogFlattenTest, StringLeafViewStopsAtFirstNul) {
  // PX4 zero-pads short strings; the spec says no terminator is REQUIRED, so a
  // full-width string has none. Both must decode correctly.
  const std::vector<uint8_t> raw = {0, 0, 'a', 'b', 'c', 0, 0, 0, 0, 0, 'f', 'u', 'l', 'l'};
  EXPECT_EQ(ulog_flatten::stringLeafView(raw.data(), 2, 8), "abc");
  EXPECT_EQ(ulog_flatten::stringLeafView(raw.data(), 10, 4), "full");
  EXPECT_EQ(ulog_flatten::stringLeafView(raw.data(), 5, 3), "");
}

// --- Padding (ULog spec, "Padding"): "If the padding field is the last
// field, then this field may not be logged, to avoid writing unnecessary
// data. This means the message_data_s.data will be shorter by the size of
// the padding. However the padding is still needed when the message is used
// in a nested definition." ---

TEST(ULogFlattenTest, TrailingPaddingIsNotRequiredInLoggedData) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  // uint64 timestamp(8) + float value(4) + uint8[4] _padding0(4) = 16 bytes on
  // the wire, but the trailing padding is optional in the data.
  builder.writeFormat("padded:uint64_t timestamp;float value;uint8_t[4] _padding0");
  builder.writeSubscription(1, 0, "padded");

  {
    // Record omits the 4 trailing padding bytes entirely, exactly as a real
    // ULog writer does.
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000);
    fields.append<float>(3.5f);
    builder.writeData(1, fields.build());
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("padded");
  ASSERT_NE(sub, nullptr);
  ASSERT_EQ(sub->size(), 1u);

  EXPECT_EQ(sub->format()->sizeBytes(), 16);
  EXPECT_EQ(ulog_flatten::loggedSizeBytes(*sub->format()), 12u);

  const auto& raw = sub->rawSamples()[0].data();
  ASSERT_EQ(raw.size(), 12u);  // shorter than sizeBytes(), by design
  // A caller gating on the corrected minimum must accept this record.
  EXPECT_GE(raw.size(), ulog_flatten::loggedSizeBytes(*sub->format()));

  // The padding field never becomes a leaf, so nothing reads past `raw`.
  std::vector<ulog_flatten::FlatLeaf> leaves;
  ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) { leaves.push_back(leaf); });
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0].offset, 8u);
  EXPECT_EQ(leaves[0].size, 4u);
  ASSERT_LE(leaves[0].offset + leaves[0].size, raw.size());
  EXPECT_DOUBLE_EQ(decodeLeaf(raw, leaves[0].offset, leaves[0].type), 3.5);
}

TEST(ULogFlattenTest, PaddingNotLastFieldStillCountsTowardLoggedSize) {
  // "_padding0" here is followed by a real field, so it is NOT the trailing
  // field and the spec's exemption does not apply: the full size is required.
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("mid_pad:uint64_t timestamp;uint8_t[4] _padding0;float value");
  builder.writeSubscription(1, 0, "mid_pad");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("mid_pad");
  ASSERT_NE(sub, nullptr);

  EXPECT_EQ(sub->format()->sizeBytes(), 16);
  // No reduction: the last field ("value") is not padding.
  EXPECT_EQ(ulog_flatten::loggedSizeBytes(*sub->format()), 16u);
}

TEST(ULogFlattenTest, FormatWithoutTrailingPaddingIsUnaffected) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("no_pad:uint64_t timestamp;float x;float y");
  builder.writeSubscription(1, 0, "no_pad");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("no_pad");
  ASSERT_NE(sub, nullptr);

  EXPECT_EQ(ulog_flatten::loggedSizeBytes(*sub->format()), static_cast<size_t>(sub->format()->sizeBytes()));
}

TEST(ULogFlattenTest, DuplicatePaddingNamesDoNotAdmitTruncatedTimestamp) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  // ulog_cpp accepts duplicate names, but sizeBytes() counts each name only
  // once. The timestamp still occupies bytes 8-15 in the ordered layout.
  builder.writeFormat("duplicate:float value;uint8_t[4] _padding0;uint64_t timestamp;uint8_t[4] _padding0");
  builder.writeSubscription(1, 0, "duplicate");
  for (size_t size : {12u, 15u, 16u, 20u}) {
    builder.writeData(1, std::vector<uint8_t>(size, 0));
  }

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  ASSERT_TRUE(container->parsingErrors().empty());
  auto sub = container->subscription("duplicate");
  ASSERT_NE(sub, nullptr);
  const auto minimum_size = ulog_flatten::loggedSizeBytes(*sub->format());
  EXPECT_EQ(minimum_size, 16u);
  EXPECT_EQ(ulog_flatten::findTimestampOffset(*sub->format()), 8u);

  const auto& samples = sub->rawSamples();
  ASSERT_EQ(samples.size(), 4u);
  EXPECT_LT(samples[0].data().size(), minimum_size);
  EXPECT_LT(samples[1].data().size(), minimum_size);
  EXPECT_GE(samples[2].data().size(), minimum_size);
  EXPECT_GE(samples[3].data().size(), minimum_size);
}

TEST(ULogFlattenTest, NestedArrayPaddingStillCountsTowardLoggedSize) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("inner:float value;uint8_t[4] _padding0");
  builder.writeFormat("outer:uint64_t timestamp;inner[2] children;float tail;uint8_t[4] _padding0");
  builder.writeSubscription(1, 0, "outer");

  auto container = parseBuilder(builder);
  ASSERT_FALSE(container->hadFatalError());
  auto sub = container->subscription("outer");
  ASSERT_NE(sub, nullptr);
  // Both nested padding fields remain; only the outermost 4 bytes are optional.
  EXPECT_EQ(ulog_flatten::loggedSizeBytes(*sub->format()), 28u);
  std::vector<size_t> offsets;
  ulog_flatten::forEachFlatLeaf(
      *sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) { offsets.push_back(leaf.offset); });
  EXPECT_EQ(offsets, (std::vector<size_t>{8, 16, 24}));
}

// --- Parameter changes over time (PlotJuggler#1245) ---
//
// PARAMETER messages in the data section carry no timestamp of their own. The
// container stamps each one with the timestamp of the most recent data message,
// which is the best approximation the file format allows.

TEST(ULogContainerTest, ChangedParameterIsStampedWithLastDataTimestamp) {
  ULogBuilder builder;
  builder.writeHeader(500000);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeParameterInt("SYS_AUTOSTART", 4001);
  builder.writeSubscription(1, 0, "dummy");

  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(2000000);
    fields.append<float>(1.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("SYS_AUTOSTART", 4002);
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(3000000);
    fields.append<float>(2.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterFloat("MC_ROLLRATE_P", 0.25f);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  // Initial snapshot is untouched.
  ASSERT_EQ(container->initialParameters().count("SYS_AUTOSTART"), 1u);
  EXPECT_EQ(container->initialParameters().at("SYS_AUTOSTART").value().as<int32_t>(), 4001);

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].parameter.field().name(), "SYS_AUTOSTART");
  EXPECT_EQ(changes[0].parameter.value().as<int32_t>(), 4002);
  EXPECT_EQ(changes[0].timestamp_us, 2000000u);
  EXPECT_EQ(changes[1].parameter.field().name(), "MC_ROLLRATE_P");
  EXPECT_FLOAT_EQ(changes[1].parameter.value().as<float>(), 0.25f);
  EXPECT_EQ(changes[1].timestamp_us, 3000000u);
}

TEST(ULogContainerTest, ChangeBeforeAnyDataFallsBackToFileStartTimestamp) {
  ULogBuilder builder;
  builder.writeHeader(500000);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  builder.writeParameterInt("EARLY", 1);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].timestamp_us, 500000u);
}

TEST(ULogContainerTest, ClockTracksTimestampFieldWhereverItSits) {
  // Timestamp is NOT the first field here; the clock must still follow it.
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("odd:uint8_t mode;uint64_t timestamp");
  builder.writeSubscription(1, 0, "odd");
  {
    FieldDataBuilder fields;
    fields.append<uint8_t>(7);
    fields.append<uint64_t>(4000000);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("LATE", 1);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].timestamp_us, 4000000u);
}

TEST(ULogContainerTest, ClockNeverRunsBackwardsAcrossSubscriptions) {
  // Data messages are in LOG order, but timestamps are only monotonic per
  // subscription: a low-rate topic can be flushed after a high-rate one with an
  // OLDER timestamp. Parameter stamps must still never go backwards, or the
  // change history on `_parameters/<name>` is inverted.
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("fast:uint64_t timestamp;float a");
  builder.writeFormat("slow:uint64_t timestamp;float b");
  builder.writeSubscription(1, 0, "fast");
  builder.writeSubscription(2, 0, "slow");
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(5000000);
    fields.append<float>(1.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("P", 1);
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(3000000);  // lagging topic, older timestamp
    fields.append<float>(2.0f);
    builder.writeData(2, fields.build());
  }
  builder.writeParameterInt("P", 2);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].timestamp_us, 5000000u);
  // Same parameter: clamped to its own previous stamp, never earlier.
  EXPECT_EQ(changes[1].timestamp_us, 5000000u);
}

TEST(ULogContainerTest, ClockIgnoresTimestampsBelowFileStart) {
  // Real logs contain zero / pre-boot timestamps (the committed fixture has a
  // zero one). Those must not drag a parameter change before the file start,
  // where the initial snapshot lives.
  ULogBuilder builder;
  builder.writeHeader(500000);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(0);
    fields.append<float>(1.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("P", 1);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].timestamp_us, 500000u);
}

TEST(ULogContainerTest, FutureOutlierDoesNotPoisonLaterChanges) {
  // A single absurd timestamp must not pin every later parameter change to it
  // (which a global running maximum would do).
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(1000000000000ULL);  // ~11 days: outlier
    fields.append<float>(1.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("Q", 1);
  {
    FieldDataBuilder fields;
    fields.append<uint64_t>(6000000);
    fields.append<float>(2.0f);
    builder.writeData(1, fields.build());
  }
  builder.writeParameterInt("P", 1);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  const auto& changes = container->timedChangedParameters();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].parameter.field().name(), "Q");
  EXPECT_EQ(changes[1].parameter.field().name(), "P");
  EXPECT_EQ(changes[1].timestamp_us, 6000000u);
}

TEST(ULogContainerTest, FinalParametersOverlayChangesInFileOrder) {
  // The Properties dialog must show the LAST value of each parameter, as the
  // original plugin did (it overwrote on every data-section PARAMETER).
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeParameterInt("A", 1);
  builder.writeParameterInt("B", 2);
  builder.writeSubscription(1, 0, "dummy");
  builder.writeParameterInt("A", 5);
  builder.writeParameterInt("A", 7);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  ASSERT_FALSE(container->hadFatalError());

  EXPECT_EQ(container->initialParameters().at("A").value().as<int32_t>(), 1);

  const auto final_params = container->finalParameters();
  ASSERT_EQ(final_params.size(), 2u);
  EXPECT_EQ(final_params.at("A").value().as<int32_t>(), 7);
  EXPECT_EQ(final_params.at("B").value().as<int32_t>(), 2);
}

// --- Truncated files (PlotJuggler#1370 / #1419) ---
//
// A file cut mid-message must neither crash nor be reported as fatal: every
// message decoded before the cut is kept, and the reader flags nothing at all
// for a clean tail cut (it simply waits for more bytes that never come).

TEST(ULogContainerTest, TruncatedTailKeepsEverythingBeforeTheCut) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  for (uint64_t i = 1; i <= 3; ++i) {
    FieldDataBuilder fields;
    fields.append<uint64_t>(i * 1000000);
    fields.append<float>(static_cast<float>(i));
    builder.writeData(1, fields.build());
  }

  // Cut the last data message in half (header + 2 + 8 + 4 = 17 bytes per message).
  std::vector<uint8_t> cut(builder.data().begin(), builder.data().end() - 6);

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  reader.readChunk(cut.data(), static_cast<int>(cut.size()));

  EXPECT_FALSE(container->hadFatalError());
  auto sub = container->subscription("dummy");
  EXPECT_EQ(sub->size(), 2u);
}

TEST(ULogContainerTest, SampleFreeContainerDropsRecordsButKeepsTheClock) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeParameterInt("P", 1);
  builder.writeSubscription(1, 0, "dummy");
  FieldDataBuilder fields;
  fields.append<uint64_t>(5000000);
  fields.append<float>(1.0F);
  builder.writeData(1, fields.build());
  builder.writeParameterInt("P", 2);
  builder.writeData(9, fields.build());  // unknown subscription: still corruption

  auto container = std::make_shared<ulog_container::ULogContainer>(false);
  ulog_cpp::Reader reader{container};
  reader.readChunk(builder.data().data(), static_cast<int>(builder.data().size()));

  EXPECT_EQ(container->subscription("dummy")->size(), 0u);
  ASSERT_EQ(container->timedChangedParameters().size(), 1u);
  EXPECT_EQ(container->timedChangedParameters()[0].timestamp_us, 5000000u);
  EXPECT_FALSE(container->parsingErrors().empty());
}

// --- Whole import, through the BUILT plugin ---
//
// The loader decodes and writes each record as the reader hands it over. These
// tests load the plugin .so and give it a host that records every call, so they
// cover what the unit tests above cannot: topic naming, the row handed to the
// host, and the stop and error paths.

struct Cell {
  bool is_null = false;
  PJ_primitive_type_t type = PJ_PRIMITIVE_TYPE_UNSPECIFIED;
  double number = 0.0;
  std::string text;
};

struct Row {
  int64_t timestamp_ns = 0;
  std::vector<Cell> cells;
};

struct RecordingHost {
  std::vector<std::string> topic_names;                    // index = handle id - 1
  std::map<std::string, std::vector<std::string>> fields;  // topic -> field names, in order
  std::map<std::string, std::vector<Row>> rows;
  std::vector<std::string> warnings;
  uint64_t bound_records = 0;
  uint64_t stop_after_records = UINT64_MAX;  // is_stop_requested turns true from here on
  bool fail_appends = false;

  static Cell toCell(bool is_null, const PJ_scalar_value_t& v) {
    Cell cell;
    cell.is_null = is_null;
    cell.type = v.type;
    if (is_null) {
      return cell;
    }
    switch (v.type) {
      case PJ_PRIMITIVE_TYPE_FLOAT32:
        cell.number = static_cast<double>(v.data.as_float32);
        break;
      case PJ_PRIMITIVE_TYPE_FLOAT64:
        cell.number = v.data.as_float64;
        break;
      case PJ_PRIMITIVE_TYPE_INT8:
        cell.number = v.data.as_int8;
        break;
      case PJ_PRIMITIVE_TYPE_INT16:
        cell.number = v.data.as_int16;
        break;
      case PJ_PRIMITIVE_TYPE_INT32:
        cell.number = v.data.as_int32;
        break;
      case PJ_PRIMITIVE_TYPE_INT64:
        cell.number = static_cast<double>(v.data.as_int64);
        break;
      case PJ_PRIMITIVE_TYPE_UINT8:
        cell.number = v.data.as_uint8;
        break;
      case PJ_PRIMITIVE_TYPE_UINT16:
        cell.number = v.data.as_uint16;
        break;
      case PJ_PRIMITIVE_TYPE_UINT32:
        cell.number = v.data.as_uint32;
        break;
      case PJ_PRIMITIVE_TYPE_UINT64:
        cell.number = static_cast<double>(v.data.as_uint64);
        break;
      case PJ_PRIMITIVE_TYPE_BOOL:
        cell.number = v.data.as_bool;
        break;
      case PJ_PRIMITIVE_TYPE_STRING:
        cell.text.assign(v.data.as_string.data, v.data.as_string.size);
        break;
      default:
        break;
    }
    return cell;
  }

  template <typename FieldValue>
  void record(PJ_topic_handle_t topic, int64_t timestamp, const FieldValue* cells, uint64_t count) {
    Row row;
    row.timestamp_ns = timestamp;
    for (uint64_t i = 0; i < count; ++i) {
      row.cells.push_back(toCell(cells[i].is_null, cells[i].value));
    }
    rows[topic_names.at(topic.id - 1)].push_back(std::move(row));
  }

  static bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    auto& self = *static_cast<RecordingHost*>(ctx);
    const std::string topic(name.data, name.size);
    auto it = std::find(self.topic_names.begin(), self.topic_names.end(), topic);
    if (it == self.topic_names.end()) {
      self.topic_names.push_back(topic);
      it = self.topic_names.end() - 1;
    }
    *out = PJ_topic_handle_t{static_cast<uint32_t>(it - self.topic_names.begin()) + 1};
    return true;
  }

  static bool ensureField(
      void* ctx, PJ_topic_handle_t topic, PJ_string_view_t name, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    auto& self = *static_cast<RecordingHost*>(ctx);
    auto& names = self.fields[self.topic_names.at(topic.id - 1)];
    names.emplace_back(name.data, name.size);
    *out = PJ_field_handle_t{topic, static_cast<uint32_t>(names.size())};
    return true;
  }

  static bool appendRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t timestamp, const PJ_named_field_value_t* cells, uint64_t count,
      PJ_error_t*) noexcept {
    static_cast<RecordingHost*>(ctx)->record(topic, timestamp, cells, count);
    return true;
  }

  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t topic, int64_t timestamp, const PJ_bound_field_value_t* cells, uint64_t count,
      PJ_error_t* error) noexcept {
    auto& self = *static_cast<RecordingHost*>(ctx);
    if (self.fail_appends) {
      PJ::sdk::fillError(error, 1, "test", "disk full");
      return false;
    }
    ++self.bound_records;
    self.record(topic, timestamp, cells, count);
    return true;
  }
};

struct ImportOutcome {
  bool ok = false;
  std::string error;
};

/// Writes `bytes` to a temp .ulg and imports it through the built plugin into `host`.
ImportOutcome importBytes(const std::vector<uint8_t>& bytes, RecordingHost& host) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("ulog_import_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ulg");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  static const PJ_source_write_host_vtable_t kWrite = [] {
    PJ_source_write_host_vtable_t vtable{};
    vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    vtable.struct_size = sizeof(vtable);
    vtable.ensure_topic = &RecordingHost::ensureTopic;
    vtable.ensure_field = &RecordingHost::ensureField;
    vtable.append_record = &RecordingHost::appendRecord;
    vtable.append_bound_record = &RecordingHost::appendBoundRecord;
    return vtable;
  }();
  static const PJ_data_source_runtime_host_vtable_t kRuntime = [] {
    PJ_data_source_runtime_host_vtable_t vtable{};
    vtable.protocol_version = 1;
    vtable.struct_size = sizeof(vtable);
    vtable.report_message = [](void* ctx, PJ_data_source_message_level_t level, PJ_string_view_t message) noexcept {
      if (level == PJ_DATA_SOURCE_MESSAGE_WARNING) {
        static_cast<RecordingHost*>(ctx)->warnings.emplace_back(message.data, message.size);
      }
    };
    vtable.progress_start = [](void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept { return true; };
    vtable.progress_update = [](void*, uint64_t) noexcept { return true; };
    vtable.progress_finish = [](void*) noexcept {};
    vtable.is_stop_requested = [](void* ctx) noexcept {
      const auto& self = *static_cast<RecordingHost*>(ctx);
      return self.bound_records >= self.stop_after_records;
    };
    vtable.notify_state = [](void*, PJ_data_source_state_t) noexcept {};
    return vtable;
  }();

  ImportOutcome outcome;
  auto library = PJ::DataSourceLibrary::load(std::string(ULOG_SOURCE_PLUGIN_PATH));
  if (!library) {
    outcome.error = "load: " + library.error();
    return outcome;
  }
  auto handle = library->createHandle();
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SourceWriteHostService>(PJ_source_write_host_t{.ctx = &host, .vtable = &kWrite});
  registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(
      PJ_data_source_runtime_host_t{.ctx = &host, .vtable = &kRuntime});
  if (auto status = handle.bind(registry.view()); !status) {
    outcome.error = "bind: " + status.error();
  } else if (status = handle.loadConfig(R"({"filepath":")" + path.generic_string() + R"("})"); !status) {
    outcome.error = "loadConfig: " + status.error();
  } else if (status = handle.start(); !status) {
    outcome.error = status.error();
  } else {
    outcome.ok = true;
  }
  std::filesystem::remove(path);
  return outcome;
}

std::vector<uint8_t> dummyRecord(uint64_t timestamp_us, float value) {
  FieldDataBuilder fields;
  fields.append<uint64_t>(timestamp_us);
  fields.append<float>(value);
  return fields.build();
}

TEST(ULogImportTest, EveryLeafTypeReachesTheHostWithItsValue) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat(
      "mix:uint64_t timestamp;int8_t i8;uint16_t u16;int32_t i32;double f64;bool flag;char[4] tag;float[2] arr;"
      "uint8_t u8;int16_t i16;uint32_t u32;int64_t i64;uint64_t u64;float f32;char c");
  builder.writeSubscription(1, 0, "mix");
  FieldDataBuilder fields;
  fields.append<uint64_t>(7000000);
  fields.append<int8_t>(-5);
  fields.append<uint16_t>(65000);
  fields.append<int32_t>(-123456);
  fields.append<double>(2.5);
  fields.append<uint8_t>(1);
  fields.append<char>('a');
  fields.append<char>('b');
  fields.append<char>('\0');
  fields.append<char>('\0');
  fields.append<float>(1.5F);
  fields.append<float>(-2.5F);
  fields.append<uint8_t>(200);
  fields.append<int16_t>(-30000);
  fields.append<uint32_t>(4000000000U);
  fields.append<int64_t>(-5000000000LL);
  fields.append<uint64_t>(6000000000ULL);
  fields.append<float>(0.25F);
  fields.append<char>('z');
  builder.writeData(1, fields.build());

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  const std::vector<std::string> expected_fields = {"i8", "u16", "i32", "f64", "flag", "tag", "arr.00", "arr.01",
                                                    "u8", "i16", "u32", "i64", "u64",  "f32", "c"};
  EXPECT_EQ(host.fields["mix"], expected_fields);
  ASSERT_EQ(host.rows["mix"].size(), 1u);
  const Row& row = host.rows["mix"][0];
  EXPECT_EQ(row.timestamp_ns, 7000000LL * 1000);
  ASSERT_EQ(row.cells.size(), 15u);
  EXPECT_EQ(row.cells[0].type, PJ_PRIMITIVE_TYPE_INT8);
  EXPECT_EQ(row.cells[0].number, -5.0);
  EXPECT_EQ(row.cells[1].type, PJ_PRIMITIVE_TYPE_UINT16);
  EXPECT_EQ(row.cells[1].number, 65000.0);
  EXPECT_EQ(row.cells[2].type, PJ_PRIMITIVE_TYPE_INT32);
  EXPECT_EQ(row.cells[2].number, -123456.0);
  EXPECT_EQ(row.cells[3].type, PJ_PRIMITIVE_TYPE_FLOAT64);
  EXPECT_EQ(row.cells[3].number, 2.5);
  EXPECT_EQ(row.cells[4].type, PJ_PRIMITIVE_TYPE_BOOL);
  EXPECT_EQ(row.cells[4].number, 1.0);
  EXPECT_EQ(row.cells[5].type, PJ_PRIMITIVE_TYPE_STRING);
  EXPECT_EQ(row.cells[5].text, "ab");
  EXPECT_EQ(row.cells[6].type, PJ_PRIMITIVE_TYPE_FLOAT32);
  EXPECT_EQ(row.cells[6].number, 1.5);
  EXPECT_EQ(row.cells[7].number, -2.5);
  EXPECT_EQ(row.cells[8].type, PJ_PRIMITIVE_TYPE_UINT8);
  EXPECT_EQ(row.cells[8].number, 200.0);
  EXPECT_EQ(row.cells[9].type, PJ_PRIMITIVE_TYPE_INT16);
  EXPECT_EQ(row.cells[9].number, -30000.0);
  EXPECT_EQ(row.cells[10].type, PJ_PRIMITIVE_TYPE_UINT32);
  EXPECT_EQ(row.cells[10].number, 4000000000.0);
  EXPECT_EQ(row.cells[11].type, PJ_PRIMITIVE_TYPE_INT64);
  EXPECT_EQ(row.cells[11].number, -5000000000.0);
  EXPECT_EQ(row.cells[12].type, PJ_PRIMITIVE_TYPE_UINT64);
  EXPECT_EQ(row.cells[12].number, 6000000000.0);
  EXPECT_EQ(row.cells[13].type, PJ_PRIMITIVE_TYPE_FLOAT32);
  EXPECT_EQ(row.cells[13].number, 0.25);
  EXPECT_EQ(row.cells[14].type, PJ_PRIMITIVE_TYPE_UINT8);  // a scalar char is a one-byte number
  EXPECT_EQ(row.cells[14].number, static_cast<double>('z'));
  for (const Cell& cell : row.cells) {
    EXPECT_FALSE(cell.is_null);
  }
}

// The `.NN` suffix depends on the whole file: an instance added after the first
// one has already logged data must still rename both topics.
TEST(ULogImportTest, InstanceAddedLateStillSuffixesBothTopics) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeFormat("solo:uint64_t timestamp;float val");
  builder.writeFormat("silent:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  builder.writeSubscription(3, 0, "solo");
  builder.writeSubscription(4, 0, "silent");  // never logs anything
  builder.writeData(1, dummyRecord(1000000, 1.0F));
  builder.writeData(3, dummyRecord(1500000, 9.0F));
  builder.writeSubscription(2, 1, "dummy");
  builder.writeData(2, dummyRecord(2000000, 2.0F));
  builder.writeData(1, dummyRecord(3000000, 3.0F));  // interleaved with the other instance

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  ASSERT_EQ(host.rows["dummy.00"].size(), 2u);
  EXPECT_EQ(host.rows["dummy.00"][0].cells.at(0).number, 1.0);
  EXPECT_EQ(host.rows["dummy.00"][1].cells.at(0).number, 3.0);
  EXPECT_EQ(host.rows["dummy.00"][1].timestamp_ns, 3000000LL * 1000);
  ASSERT_EQ(host.rows["dummy.01"].size(), 1u);
  EXPECT_EQ(host.rows["dummy.01"][0].cells.at(0).number, 2.0);
  EXPECT_EQ(host.rows["solo"].size(), 1u);  // single instance: no suffix
  EXPECT_EQ(host.rows.count("dummy"), 0u);
  // A subscription without data leaves no empty topic behind.
  EXPECT_EQ(std::count(host.topic_names.begin(), host.topic_names.end(), "silent"), 0);
}

TEST(ULogImportTest, ShortRecordIsSkippedAndMissingTimestampUsesTheSampleIndex) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeFormat("nots:float val");
  builder.writeSubscription(1, 0, "dummy");
  builder.writeSubscription(2, 0, "nots");
  builder.writeData(1, dummyRecord(1000000, 1.0F));
  builder.writeData(1, {0x01, 0x02, 0x03, 0x04});  // shorter than the format
  builder.writeData(1, dummyRecord(2000000, 2.0F));
  for (float v : {10.0F, 11.0F, 12.0F}) {
    FieldDataBuilder fields;
    fields.append<float>(v);
    builder.writeData(2, fields.build());
  }

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  ASSERT_EQ(host.rows["dummy"].size(), 2u);
  EXPECT_EQ(host.rows["dummy"][1].cells.at(0).number, 2.0);
  ASSERT_EQ(host.rows["nots"].size(), 3u);
  EXPECT_EQ(host.rows["nots"][0].timestamp_ns, 0);
  EXPECT_EQ(host.rows["nots"][2].timestamp_ns, 2000);  // sample index, in microseconds
  EXPECT_EQ(host.rows["nots"][2].cells.at(0).number, 12.0);

  const auto warned = [&](const std::string& needle) {
    return std::any_of(host.warnings.begin(), host.warnings.end(), [&](const std::string& w) {
      return w.find(needle) != std::string::npos;
    });
  };
  EXPECT_TRUE(warned("skipped 1 data record(s)"));
  EXPECT_TRUE(warned("no timestamp field"));
}

std::vector<uint8_t> manyDummyRecords(uint64_t count) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeParameterInt("P", 1);
  builder.writeSubscription(1, 0, "dummy");
  for (uint64_t i = 1; i <= count; ++i) {
    builder.writeData(1, dummyRecord(i * 1000, static_cast<float>(i)));
  }
  return builder.data();
}

// 20000 records span several 64 KiB read chunks; the stop is polled per chunk.
// Logged messages keep their scalar `_log` topic. This harness has no object
// store, so the kLog objects are skipped with a warning (the fallback path).
TEST(ULogImportTest, LoggedMessagesKeepTheScalarLogTopic) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  builder.writeData(1, dummyRecord(1000000, 1.0F));
  builder.writeLogging('3', 1500000, "Arming denied");
  builder.writeTaggedLogging('6', 7, 2500000, "Takeoff");

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  const std::vector<std::string> expected_fields = {"level", "message"};
  EXPECT_EQ(host.fields["_log"], expected_fields);
  const auto& rows = host.rows["_log"];
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].timestamp_ns, 1500000LL * 1000);
  EXPECT_EQ(rows[0].cells.at(0).text, "Error");
  EXPECT_EQ(rows[0].cells.at(1).text, "Arming denied");
  EXPECT_EQ(rows[1].timestamp_ns, 2500000LL * 1000);
  EXPECT_EQ(rows[1].cells.at(0).text, "Info");
  EXPECT_EQ(rows[1].cells.at(1).text, "Takeoff");
  EXPECT_TRUE(std::any_of(host.warnings.begin(), host.warnings.end(), [](const std::string& w) {
    return w.find("log messages imported as scalars only") != std::string::npos;
  }));
}

TEST(ULogImportTest, StopKeepsTheRecordsAlreadyWritten) {
  RecordingHost host;
  host.stop_after_records = 1;
  const auto outcome = importBytes(manyDummyRecords(20000), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  const size_t written = host.rows["dummy"].size();
  ASSERT_GT(written, 0u);
  EXPECT_LT(written, 20000u);
  EXPECT_EQ(host.rows["dummy"].back().cells.at(0).number, static_cast<double>(written));  // a clean prefix
  EXPECT_EQ(host.rows["_parameters/P"].size(), 1u);  // the cheap tail is still written
}

// The scan for topic names comes first and writes nothing. A stop that lands
// there must still keep the part of the file read so far, as it always did.
TEST(ULogImportTest, StopDuringTheNameScanKeepsThePrefix) {
  RecordingHost host;
  host.stop_after_records = 0;  // stop is already requested at the first poll
  const auto outcome = importBytes(manyDummyRecords(20000), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  const size_t written = host.rows["dummy"].size();
  ASSERT_GT(written, 0u);
  EXPECT_LT(written, 20000u);
  EXPECT_EQ(host.rows["dummy"].back().cells.at(0).number, static_cast<double>(written));
}

// ulog_cpp keeps only the first subscription for a (name, multi_id) pair in its
// by-name table; the second one's records were never imported and still aren't.
TEST(ULogImportTest, DuplicateSubscriptionDoesNotMergeIntoTheFirst) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dummy:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "dummy");
  builder.writeSubscription(2, 0, "dummy");
  builder.writeData(1, dummyRecord(1000000, 1.0F));
  builder.writeData(2, dummyRecord(1500000, 99.0F));
  builder.writeData(1, dummyRecord(2000000, 2.0F));

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;

  ASSERT_EQ(host.rows["dummy"].size(), 2u);
  EXPECT_EQ(host.rows["dummy"][0].cells.at(0).number, 1.0);
  EXPECT_EQ(host.rows["dummy"][1].cells.at(0).number, 2.0);
  EXPECT_EQ(host.fields["dummy"].size(), 1u);
}

// A format whose nested type is never defined stops ulog_cpp resolving every
// format after it. Records of such a subscription cannot be decoded: the
// import fails, it does not pass them off as file corruption.
TEST(ULogImportTest, UnresolvedFormatFailsTheImport) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("a_bad:uint64_t timestamp;missing_t m");
  builder.writeFormat("b_ok:uint64_t timestamp;float val");
  builder.writeFormat("c_ok:uint64_t timestamp;float val");
  builder.writeSubscription(1, 0, "b_ok");  // lost: it is the message that trips the failed resolve
  builder.writeSubscription(2, 0, "c_ok");
  for (uint64_t i = 1; i <= 5000; ++i) {
    builder.writeData(2, dummyRecord(i * 1000, static_cast<float>(i)));
  }

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("Unresolved"), std::string::npos) << outcome.error;
  EXPECT_TRUE(host.rows["c_ok"].empty());
}

// MessageFormat::sizeBytes() under-counts a format with duplicate field names,
// so a record can pass the size gate and still end before its timestamp.
TEST(ULogImportTest, RecordEndingBeforeItsTimestampIsSkipped) {
  ULogBuilder builder;
  builder.writeHeader(0);
  builder.writeFlagBits();
  builder.writeFormat("dup:uint32_t a;uint32_t a;uint64_t timestamp");
  builder.writeSubscription(1, 0, "dup");
  FieldDataBuilder fields;  // 12 bytes: the timestamp would sit at bytes 8..16
  fields.append<uint32_t>(1);
  fields.append<uint32_t>(2);
  fields.append<uint32_t>(3);
  builder.writeData(1, fields.build());

  RecordingHost host;
  const auto outcome = importBytes(builder.data(), host);
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_TRUE(host.rows["dup"].empty());
  EXPECT_TRUE(std::any_of(host.warnings.begin(), host.warnings.end(), [](const std::string& w) {
    return w.find("skipped 1 data record(s)") != std::string::npos;
  }));
}

TEST(ULogImportTest, HostWriteErrorFailsTheImport) {
  RecordingHost host;
  host.fail_appends = true;
  const auto outcome = importBytes(manyDummyRecords(10), host);
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("disk full"), std::string::npos) << outcome.error;
}

// --- Real PX4 log (committed fixture) ---
//
// Drives the new code paths over a genuine flight log, fed in 64 KiB chunks
// exactly as the importer does, so chunk-boundary handling is covered too.

TEST(ULogContainerTest, SampleLogExercisesAllNewPaths) {
  std::ifstream file(ULOG_TEST_DATA_DIR "/sample_log_small.ulg", std::ios::binary);
  ASSERT_TRUE(file.is_open());

  auto container = std::make_shared<ulog_container::ULogContainer>();
  ulog_cpp::Reader reader{container};
  std::vector<uint8_t> buffer(65536);
  while (file) {
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    auto count = static_cast<size_t>(file.gcount());
    if (count == 0) {
      break;
    }
    reader.readChunk(buffer.data(), static_cast<int>(count));
  }
  ASSERT_FALSE(container->hadFatalError());
  EXPECT_TRUE(container->parsingErrors().empty());

  // Every subscribed format carries a locatable timestamp, and PX4 puts it
  // first — the by-name lookup must agree with the old fixed-offset read here.
  size_t sub_count = 0;
  for (const auto& [key, sub] : container->subscriptionsByNameAndMultiId()) {
    ++sub_count;
    auto offset = ulog_flatten::findTimestampOffset(*sub->format());
    ASSERT_TRUE(offset.has_value()) << key.name;
    EXPECT_EQ(*offset, 0u) << key.name;
  }
  EXPECT_EQ(sub_count, 72u);

  // Names and leaves stay zipped for every format, strings included.
  for (const auto& [name, fmt] : container->messageFormats()) {
    std::vector<std::string> names;
    ulog_flatten::collectFlatFieldNames(*fmt, {}, names);
    size_t leaf_count = 0;
    ulog_flatten::forEachFlatLeaf(*fmt, 0, [&](const ulog_flatten::FlatLeaf&) { ++leaf_count; });
    EXPECT_EQ(names.size(), leaf_count) << name;
  }

  // This particular log never changes a parameter in flight: the snapshot is
  // large and the change list is empty (the synthetic tests cover changes).
  EXPECT_EQ(container->initialParameters().size(), 980u);
  EXPECT_TRUE(container->timedChangedParameters().empty());

  // `vehicle_local_position`'s format ends in a trailing `uint8_t[4]
  // _padding0`: every one of its 636 real PX4 records is logged 4 bytes
  // shorter than sizeBytes() (156 vs. 160). Before the padding fix, the
  // importer's `raw.size() < format_size` gate rejected all of them, so the
  // topic came out completely empty; loggedSizeBytes() must recognize every
  // record as valid.
  auto local_pos = container->subscription("vehicle_local_position");
  ASSERT_NE(local_pos, nullptr);
  EXPECT_EQ(local_pos->format()->sizeBytes(), 160);
  const auto logged_size = ulog_flatten::loggedSizeBytes(*local_pos->format());
  EXPECT_EQ(logged_size, 156u);
  ASSERT_EQ(local_pos->rawSamples().size(), 636u);
  for (const auto& sample : local_pos->rawSamples()) {
    EXPECT_EQ(sample.data().size(), logged_size);
    EXPECT_GE(sample.data().size(), logged_size);
  }
}

}  // namespace
