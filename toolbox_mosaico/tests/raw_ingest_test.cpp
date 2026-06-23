// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "../src/raw_ingest.hpp"

#include <arrow/api.h>
#include <arrow/array/builder_binary.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct BindingRecord {
  std::string topic_name;
  std::string parser_encoding;
  std::string type_name;
  std::vector<std::uint8_t> schema;
  std::string parser_config_json;
};

struct MessageRecord {
  std::uint32_t binding_id = 0;
  std::int64_t timestamp_ns = 0;
  std::vector<std::uint8_t> payload;
};

struct FakeParserIngest {
  std::vector<BindingRecord> bindings;
  std::vector<MessageRecord> messages;
  std::uint32_t next_binding_id = 1;
  std::mutex mu;

  PJ::ParserIngestHostView view() {
    PJ_data_source_runtime_host_t host{};
    host.ctx = this;
    host.vtable = &kVtable;
    return PJ::ParserIngestHostView(host);
  }

  static FakeParserIngest* self(void* ctx) {
    return static_cast<FakeParserIngest*>(ctx);
  }
  static std::string toStr(PJ_string_view_t s) {
    return (s.data != nullptr && s.size > 0) ? std::string(s.data, s.size) : std::string();
  }

  static void reportMessage(void*, PJ_data_source_message_level_t, PJ_string_view_t) PJ_NOEXCEPT {}
  static bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) PJ_NOEXCEPT {
    return true;
  }
  static bool progressUpdate(void*, uint64_t) PJ_NOEXCEPT {
    return true;
  }
  static void progressFinish(void*) PJ_NOEXCEPT {}
  static bool isStopRequested(void*) PJ_NOEXCEPT {
    return false;
  }
  static void notifyState(void*, PJ_data_source_state_t) PJ_NOEXCEPT {}
  static void requestStop(void*, PJ_data_source_state_t, PJ_string_view_t) PJ_NOEXCEPT {}
  static int showMessageBox(void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) PJ_NOEXCEPT {
    return PJ_MSG_BTN_OK;
  }
  static const char* listAvailableEncodings(void*) PJ_NOEXCEPT {
    return R"(["ros2msg","protobuf"])";
  }

  static bool ensureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out_handle,
      PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lock(h->mu);
    BindingRecord rec;
    rec.topic_name = toStr(request->topic_name);
    rec.parser_encoding = toStr(request->parser_encoding);
    rec.type_name = toStr(request->type_name);
    if (request->schema.data != nullptr && request->schema.size > 0) {
      rec.schema.assign(request->schema.data, request->schema.data + request->schema.size);
    }
    rec.parser_config_json = toStr(request->parser_config_json);
    const std::uint32_t id = h->next_binding_id++;
    h->bindings.push_back(std::move(rec));
    out_handle->id = id;
    return true;
  }

  static bool pushMessage(
      void* ctx, PJ_parser_binding_handle_t handle, int64_t host_timestamp_ns,
      PJ_message_data_fetcher_t fetch_message_data, PJ_error_t* out_error) PJ_NOEXCEPT {
    PJ_payload_t payload{};
    const bool fetched = fetch_message_data.fetchMessageData(fetch_message_data.ctx, &payload, out_error);
    if (!fetched) {
      fetch_message_data.release(fetch_message_data.ctx);
      return false;
    }
    auto* h = self(ctx);
    MessageRecord rec;
    rec.binding_id = handle.id;
    rec.timestamp_ns = host_timestamp_ns;
    if (payload.data != nullptr && payload.size > 0) {
      rec.payload.assign(payload.data, payload.data + payload.size);
    }
    if (payload.anchor.release != nullptr) {
      payload.anchor.release(payload.anchor.ctx);
    }
    fetch_message_data.release(fetch_message_data.ctx);
    std::lock_guard<std::mutex> lock(h->mu);
    h->messages.push_back(std::move(rec));
    return true;
  }

  static const PJ_data_source_runtime_host_vtable_t kVtable;
};

const PJ_data_source_runtime_host_vtable_t FakeParserIngest::kVtable = [] {
  PJ_data_source_runtime_host_vtable_t v{};
  v.protocol_version = 1;
  v.struct_size = sizeof(PJ_data_source_runtime_host_vtable_t);
  v.report_message = &FakeParserIngest::reportMessage;
  v.progress_start = &FakeParserIngest::progressStart;
  v.progress_update = &FakeParserIngest::progressUpdate;
  v.progress_finish = &FakeParserIngest::progressFinish;
  v.is_stop_requested = &FakeParserIngest::isStopRequested;
  v.notify_state = &FakeParserIngest::notifyState;
  v.request_stop = &FakeParserIngest::requestStop;
  v.ensure_parser_binding = &FakeParserIngest::ensureParserBinding;
  v.show_message_box = &FakeParserIngest::showMessageBox;
  v.list_available_encodings = &FakeParserIngest::listAvailableEncodings;
  v.push_message = &FakeParserIngest::pushMessage;
  return v;
}();

struct RawRow {
  std::int64_t timestamp_ns = 0;
  std::optional<std::vector<std::uint8_t>> payload;
};

std::shared_ptr<arrow::Table> makeRawTable(const std::vector<RawRow>& rows) {
  arrow::Int64Builder ts_b;
  arrow::BinaryViewBuilder payload_b;
  for (const auto& row : rows) {
    EXPECT_TRUE(ts_b.Append(row.timestamp_ns).ok());
    if (row.payload.has_value()) {
      EXPECT_TRUE(payload_b.Append(row.payload->data(), static_cast<int64_t>(row.payload->size())).ok());
    } else {
      EXPECT_TRUE(payload_b.AppendNull().ok());
    }
  }

  std::shared_ptr<arrow::Array> ts_a;
  std::shared_ptr<arrow::Array> payload_a;
  EXPECT_TRUE(ts_b.Finish(&ts_a).ok());
  EXPECT_TRUE(payload_b.Finish(&payload_a).ok());

  auto schema = arrow::schema({
      arrow::field("timestamp_ns", arrow::int64()),
      arrow::field("payload", arrow::binary_view()),
  });
  return arrow::Table::Make(schema, {ts_a, payload_a});
}

}  // namespace

TEST(RawIngestTest, BuildsRos2ParserConfigFromMcapMetadata) {
  const std::unordered_map<std::string, std::string> metadata = {
      {"pj.raw.kind", "mcap_channel"},
      {"pj.topic_name", "/tf"},
      {"pj.topic_type", "tf2_msgs/msg/TFMessage"},
      {"mcap.channel.message_encoding", "cdr"},
      {"mcap.schema.encoding", "ros2msg"},
      {"pj.schema_b64", "AQID"},
  };

  auto config = mosaico::rawParserTopicConfig("/fallback", metadata);
  ASSERT_TRUE(config.has_value()) << config.error();
  EXPECT_EQ(config->topic_name, "/tf");
  EXPECT_EQ(config->type_name, "tf2_msgs/msg/TFMessage");
  EXPECT_EQ(config->parser_encoding, "ros2msg");
  EXPECT_EQ(config->serialization, "cdr");
  EXPECT_EQ(config->schema_encoding, "ros2msg");
  EXPECT_EQ(config->schema, (std::vector<std::uint8_t>{1, 2, 3}));

  const auto json = nlohmann::json::parse(config->parser_config_json);
  EXPECT_EQ(json["topic_name"], "/tf");
  EXPECT_EQ(json["serialization"], "cdr");
  EXPECT_EQ(json["schema_encoding"], "ros2msg");
}

TEST(RawIngestTest, BuildsProtobufParserConfigAndPreservesOverrides) {
  const std::unordered_map<std::string, std::string> metadata = {
      {"pj.raw.kind", "mcap_channel"},
      {"pj.topic_type", "foxglove.PointCloud"},
      {"mcap.channel.message_encoding", "protobuf"},
      {"mcap.schema.encoding", "protobuf"},
      {"pj.parser_config_json", R"({"use_embedded_timestamp":true})"},
  };

  auto config = mosaico::rawParserTopicConfig("/pointcloud", metadata);
  ASSERT_TRUE(config.has_value()) << config.error();
  EXPECT_EQ(config->topic_name, "/pointcloud");
  EXPECT_EQ(config->parser_encoding, "protobuf");
  EXPECT_EQ(config->serialization, "protobuf");

  const auto json = nlohmann::json::parse(config->parser_config_json);
  EXPECT_TRUE(json["use_embedded_timestamp"]);
  EXPECT_EQ(json["topic_name"], "/pointcloud");
  EXPECT_EQ(json["serialization"], "protobuf");
  EXPECT_EQ(json["schema_encoding"], "protobuf");
}

TEST(RawIngestTest, PushesPayloadRowsThroughParserIngest) {
  mosaico::RawParserTopicConfig config;
  config.topic_name = "/lidar/front/rslidar_points";
  config.type_name = "sensor_msgs/msg/PointCloud2";
  config.parser_encoding = "ros2msg";
  config.serialization = "cdr";
  config.schema_encoding = "ros2msg";
  config.schema = {9, 8, 7};
  config.parser_config_json =
      R"({"serialization":"cdr","schema_encoding":"ros2msg","topic_name":"/lidar/front/rslidar_points"})";

  auto table = makeRawTable({
      {.timestamp_ns = 100, .payload = std::vector<std::uint8_t>{1, 2, 3}},
      {.timestamp_ns = 101, .payload = std::nullopt},
      {.timestamp_ns = 102, .payload = std::vector<std::uint8_t>{}},
  });
  FakeParserIngest fake;

  auto pushed = mosaico::pushRawRowsToParser(fake.view(), config, table);
  ASSERT_TRUE(pushed.has_value()) << pushed.error();
  EXPECT_EQ(pushed->pushed, 2);
  EXPECT_EQ(pushed->skipped, 1);
  EXPECT_NE(pushed->first_error.find("missing payload"), std::string::npos);

  ASSERT_EQ(fake.bindings.size(), 1U);
  EXPECT_EQ(fake.bindings[0].topic_name, "/lidar/front/rslidar_points");
  EXPECT_EQ(fake.bindings[0].parser_encoding, "ros2msg");
  EXPECT_EQ(fake.bindings[0].type_name, "sensor_msgs/msg/PointCloud2");
  EXPECT_EQ(fake.bindings[0].schema, (std::vector<std::uint8_t>{9, 8, 7}));

  ASSERT_EQ(fake.messages.size(), 2U);
  EXPECT_EQ(fake.messages[0].binding_id, 1U);
  EXPECT_EQ(fake.messages[0].timestamp_ns, 100);
  EXPECT_EQ(fake.messages[0].payload, (std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_EQ(fake.messages[1].timestamp_ns, 102);
  EXPECT_TRUE(fake.messages[1].payload.empty());
}
