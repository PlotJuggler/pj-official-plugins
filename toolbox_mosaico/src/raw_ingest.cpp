// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "raw_ingest.hpp"

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base64/base64.hpp>
#include <string>
#include <utility>

namespace mosaico {

namespace {

[[nodiscard]] std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

[[nodiscard]] std::string metadataValue(
    const std::unordered_map<std::string, std::string>& metadata, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (auto it = metadata.find(key); it != metadata.end() && !it->second.empty()) {
      return it->second;
    }
  }
  return {};
}

[[nodiscard]] bool isRos1(std::string_view encoding) {
  return encoding == "ros1" || encoding == "ros1msg";
}

[[nodiscard]] bool isProtobuf(std::string_view encoding) {
  return encoding == "protobuf" || encoding == "proto";
}

[[nodiscard]] std::string inferParserEncoding(
    std::string parser_encoding, const std::string& channel_encoding, const std::string& schema_encoding) {
  parser_encoding = lowerCopy(std::move(parser_encoding));
  const std::string channel = lowerCopy(channel_encoding);
  const std::string schema = lowerCopy(schema_encoding);
  if (parser_encoding.empty() || (parser_encoding == "cdr" && !schema.empty())) {
    parser_encoding = !schema.empty() ? schema : channel;
  }
  if (parser_encoding == "ros1") {
    return "ros1msg";
  }
  if (parser_encoding == "ros2") {
    return "ros2msg";
  }
  if (parser_encoding == "ros2idl" || parser_encoding == "idl") {
    return "omgidl";
  }
  return parser_encoding;
}

[[nodiscard]] std::string inferSerialization(
    std::string serialization, const std::string& channel_encoding, const std::string& parser_encoding,
    const std::string& schema_encoding) {
  serialization = lowerCopy(std::move(serialization));
  const std::string channel = lowerCopy(channel_encoding);
  const std::string parser = lowerCopy(parser_encoding);
  const std::string schema = lowerCopy(schema_encoding);
  if (!serialization.empty()) {
    return serialization;
  }
  if (isRos1(channel) || isRos1(parser) || isRos1(schema)) {
    return "ros1";
  }
  if (isProtobuf(channel) || isProtobuf(parser) || isProtobuf(schema)) {
    return "protobuf";
  }
  if (!channel.empty()) {
    return channel;
  }
  return "cdr";
}

[[nodiscard]] std::string buildParserConfigJson(const std::string& override_json, const RawParserTopicConfig& config) {
  nlohmann::json json = nlohmann::json::object();
  if (!override_json.empty()) {
    auto parsed = nlohmann::json::parse(override_json, nullptr, false);
    if (parsed.is_object()) {
      json = std::move(parsed);
    }
  }
  json["serialization"] = config.serialization;
  json["schema_encoding"] = config.schema_encoding.empty() ? config.parser_encoding : config.schema_encoding;
  json["topic_name"] = config.topic_name;
  return json.dump();
}

[[nodiscard]] std::string detectTimestampField(const std::shared_ptr<arrow::Schema>& schema) {
  if (!schema) {
    return {};
  }
  for (const auto& field : schema->fields()) {
    if (field && field->type()->id() == arrow::Type::TIMESTAMP) {
      return field->name();
    }
  }
  static constexpr std::array<std::string_view, 5> kNames = {
      "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (std::string_view name : kNames) {
    if (schema->GetFieldByName(std::string(name)) != nullptr) {
      return std::string(name);
    }
  }
  return {};
}

[[nodiscard]] std::optional<std::int64_t> arrowI64At(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return std::nullopt;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return std::nullopt;
      }
      switch (chunk->type_id()) {
        case arrow::Type::INT64:
          return std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT64:
          return static_cast<std::int64_t>(std::static_pointer_cast<arrow::UInt64Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT32:
          return std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT32:
          return static_cast<std::int64_t>(std::static_pointer_cast<arrow::UInt32Array>(chunk)->Value(chunk_row));
        case arrow::Type::TIMESTAMP:
          return std::static_pointer_cast<arrow::TimestampArray>(chunk)->Value(chunk_row);
        default:
          return std::nullopt;
      }
    }
    chunk_row -= chunk->length();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> arrowBinaryCopyAt(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return std::nullopt;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return std::nullopt;
      }
      const std::uint8_t* ptr = nullptr;
      std::size_t size = 0;
      switch (chunk->type_id()) {
        case arrow::Type::BINARY: {
          auto bin = std::static_pointer_cast<arrow::BinaryArray>(chunk);
          int32_t length = 0;
          ptr = bin->GetValue(chunk_row, &length);
          size = static_cast<std::size_t>(length);
          break;
        }
        case arrow::Type::LARGE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::LargeBinaryArray>(chunk);
          int64_t length = 0;
          ptr = bin->GetValue(chunk_row, &length);
          size = static_cast<std::size_t>(length);
          break;
        }
        case arrow::Type::FIXED_SIZE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(chunk);
          ptr = bin->GetValue(chunk_row);
          size = static_cast<std::size_t>(bin->byte_width());
          break;
        }
        case arrow::Type::BINARY_VIEW: {
          auto bin = std::static_pointer_cast<arrow::BinaryViewArray>(chunk);
          const std::string_view view = bin->GetView(chunk_row);
          ptr = reinterpret_cast<const std::uint8_t*>(view.data());
          size = view.size();
          break;
        }
        default:
          return std::nullopt;
      }
      if (size == 0) {
        return std::vector<std::uint8_t>{};
      }
      return std::vector<std::uint8_t>(ptr, ptr + size);
    }
    chunk_row -= chunk->length();
  }
  return std::nullopt;
}

}  // namespace

bool isRawMcapMetadata(const std::unordered_map<std::string, std::string>& metadata) {
  const std::string kind = lowerCopy(metadataValue(metadata, {"pj.raw.kind", "raw.kind"}));
  return kind == "mcap_channel";
}

PJ::Expected<RawParserTopicConfig> rawParserTopicConfig(
    std::string_view fallback_topic_name, const std::unordered_map<std::string, std::string>& metadata) {
  if (!isRawMcapMetadata(metadata)) {
    return PJ::unexpected("metadata does not describe a raw MCAP channel");
  }

  RawParserTopicConfig config;
  config.topic_name = metadataValue(metadata, {"pj.topic_name", "topic_name", "mcap.channel.topic"});
  if (config.topic_name.empty()) {
    config.topic_name = std::string(fallback_topic_name);
  }
  config.type_name = metadataValue(metadata, {"pj.topic_type", "topic_type", "mcap.schema.name", "schema.name"});
  const std::string schema_encoding =
      metadataValue(metadata, {"pj.schema_encoding", "schema_encoding", "mcap.schema.encoding", "schema.encoding"});
  const std::string channel_encoding = metadataValue(
      metadata, {"mcap.channel.message_encoding", "message_encoding", "channel_encoding", "pj.message_encoding",
                 "pj.serialization", "serialization"});
  config.schema_encoding = lowerCopy(schema_encoding);
  config.parser_encoding = inferParserEncoding(
      metadataValue(metadata, {"pj.parser_encoding", "parser_encoding"}), channel_encoding, config.schema_encoding);
  config.serialization = inferSerialization(
      metadataValue(metadata, {"pj.serialization", "serialization"}), channel_encoding, config.parser_encoding,
      config.schema_encoding);

  if (config.topic_name.empty()) {
    return PJ::unexpected("raw MCAP metadata is missing topic name");
  }
  if (config.type_name.empty()) {
    return PJ::unexpected("raw MCAP metadata is missing topic type/schema name");
  }
  if (config.parser_encoding.empty()) {
    return PJ::unexpected("raw MCAP metadata is missing parser/schema encoding");
  }

  const std::string schema_b64 = metadataValue(metadata, {"pj.schema_b64", "schema_b64", "mcap.schema.data_b64"});
  if (!schema_b64.empty()) {
    const std::string decoded = PJ::base64::decode(schema_b64);
    config.schema.assign(decoded.begin(), decoded.end());
  }

  config.parser_config_json =
      buildParserConfigJson(metadataValue(metadata, {"pj.parser_config_json", "parser_config_json"}), config);
  return config;
}

PJ::Expected<RawParserPushOutcome> pushRawRowsToParser(
    const PJ::ParserIngestHostView& ingest, const RawParserTopicConfig& config,
    const std::shared_ptr<arrow::Table>& table) {
  if (!ingest.valid()) {
    return PJ::unexpected("raw parser ingest host is not bound");
  }
  if (!table) {
    return PJ::unexpected("raw parser topic '" + config.topic_name + "': null table");
  }
  const std::string ts_field = detectTimestampField(table->schema());
  if (ts_field.empty()) {
    return PJ::unexpected("raw parser topic '" + config.topic_name + "' missing timestamp column");
  }
  const auto ts_col = table->GetColumnByName(ts_field);
  const auto payload_col =
      table->GetColumnByName("payload") ? table->GetColumnByName("payload") : table->GetColumnByName("data");
  if (!payload_col) {
    return PJ::unexpected("raw parser topic '" + config.topic_name + "' missing 'payload' or 'data' column");
  }

  PJ::ParserBindingRequest request{
      .topic_name = config.topic_name,
      .parser_encoding = config.parser_encoding,
      .type_name = config.type_name,
      .schema = PJ::Span<const std::uint8_t>(config.schema.data(), config.schema.size()),
      .parser_config_json = config.parser_config_json,
  };
  auto binding = ingest.ensureParserBinding(request);
  if (!binding) {
    return PJ::unexpected("raw parser topic '" + config.topic_name + "' bind failed: " + binding.error());
  }

  RawParserPushOutcome outcome;
  const std::int64_t num_rows = table->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const auto ts = arrowI64At(ts_col, row);
    if (!ts) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            "raw parser topic '" + config.topic_name + "' row " + std::to_string(row) + ": missing timestamp";
      }
      continue;
    }
    auto payload = arrowBinaryCopyAt(payload_col, row);
    if (!payload) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            "raw parser topic '" + config.topic_name + "' row " + std::to_string(row) + ": missing payload";
      }
      continue;
    }
    auto view = PJ::sdk::makePayloadView(std::move(*payload));
    auto status = ingest.pushMessage(*binding, *ts, [view]() { return view; });
    if (!status) {
      return PJ::unexpected(
          "raw parser topic '" + config.topic_name + "' row " + std::to_string(row) +
          " push failed: " + status.error());
    }
    ++outcome.pushed;
  }
  return outcome;
}

}  // namespace mosaico
