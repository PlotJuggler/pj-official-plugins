#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

  const std::vector<uint8_t>& data() const { return buf_; }

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
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    vec.insert(vec.end(), bytes, bytes + sizeof(T));
  }
};

// Helper to build field data for a data message.
class FieldDataBuilder {
 public:
  template <typename T>
  void append(T value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    data_.insert(data_.end(), bytes, bytes + sizeof(T));
  }

  std::vector<uint8_t> build() const { return data_; }

 private:
  std::vector<uint8_t> data_;
};

// Parse a ULogBuilder into a DataContainer.
std::shared_ptr<ulog_cpp::DataContainer> parseBuilder(const ULogBuilder& builder) {
  auto container =
      std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
  ulog_cpp::Reader reader{container};
  const auto& buf = builder.data();
  reader.readChunk(buf.data(), static_cast<int>(buf.size()));
  return container;
}

TEST(ULogSourceTest, InvalidDataReportsFatalError) {
  std::vector<uint8_t> bad_data = {'N', 'O', 'T', '_', 'U', 'L', 'O', 'G',
                                    0, 0, 0, 0, 0, 0, 0, 0};
  auto container =
      std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
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

}  // namespace
