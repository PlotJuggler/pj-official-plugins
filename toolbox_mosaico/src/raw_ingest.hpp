// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/c/abi.h>

#include <cstdint>
#include <memory>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arrow {
class Table;
}  // namespace arrow

namespace mosaico {

struct RawParserTopicConfig {
  std::string topic_name;
  std::string type_name;
  std::string parser_encoding;
  std::string serialization;
  std::string schema_encoding;
  std::vector<std::uint8_t> schema;
  std::string parser_config_json;
};

struct RawParserPushOutcome {
  std::int64_t pushed = 0;
  std::int64_t skipped = 0;
  std::string first_error;
};

[[nodiscard]] bool isRawMcapMetadata(const std::unordered_map<std::string, std::string>& metadata);

[[nodiscard]] PJ::Expected<RawParserTopicConfig> rawParserTopicConfig(
    std::string_view fallback_topic_name, const std::unordered_map<std::string, std::string>& metadata);

[[nodiscard]] PJ::Expected<RawParserPushOutcome> pushRawRowsToParser(
    const PJ::ParserIngestHostView& ingest, const RawParserTopicConfig& config,
    const std::shared_ptr<arrow::Table>& table);

}  // namespace mosaico
