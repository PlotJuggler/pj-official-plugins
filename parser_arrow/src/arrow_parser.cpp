#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arrow_manifest.hpp"
#include "ipc_decoder.hpp"
#include "table_shaper.hpp"

namespace {

using pj::parser_arrow::DroppedColumn;

/// Return whether two dropped-column lists contain identical name/format pairs in the same order.
[[nodiscard]] bool sameDroppedColumns(
    const std::vector<DroppedColumn>& left, const std::vector<DroppedColumn>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  return std::equal(
      left.begin(), left.end(), right.begin(), [](const DroppedColumn& left_column, const DroppedColumn& right_column) {
        return left_column.name == right_column.name && left_column.format == right_column.format;
      });
}

/// Build the bounded human-readable warning for Arrow columns removed before host ingest.
[[nodiscard]] std::string droppedColumnsMessage(const std::vector<DroppedColumn>& columns) {
  std::string message = std::to_string(columns.size());
  message += " column(s) removed from the Arrow stream (unsupported host type): ";
  constexpr std::size_t kMaxListedColumns = 8;
  const std::size_t listed_columns = std::min(columns.size(), kMaxListedColumns);
  for (std::size_t index = 0; index < listed_columns; ++index) {
    if (index > 0) {
      message += ", ";
    }
    message += columns[index].name;
    message += ":";
    message += columns[index].format;
  }
  if (listed_columns < columns.size()) {
    message += ", …";
  }
  return message;
}

/// Report a changed non-empty dropped-column set without making diagnostic failures fatal to parsing.
void reportDroppedColumnsIfChanged(
    const PJ::sdk::ParserRuntimeHostView& runtime_host, const std::vector<DroppedColumn>& columns,
    std::vector<DroppedColumn>& last_columns) noexcept {
  try {
    if (sameDroppedColumns(columns, last_columns)) {
      return;
    }
    auto next_columns = columns;
    if (!next_columns.empty()) {
      const std::string message = droppedColumnsMessage(next_columns);
      runtime_host.reportDiagnostic(
          PJ::sdk::ParserDiagnosticLevel::Warning, "parser_arrow.dropped_columns", message, 1);
    }
    last_columns.swap(next_columns);
  } catch (...) {
    // Diagnostics are non-fatal: allocation failure must not reject an otherwise ingestible message.
  }
}

/// User-configurable shaping and timestamp options for Arrow IPC streams.
struct ArrowParserConfig {
  std::string timestamp_column;
  bool flatten_structs = true;
  int64_t synthetic_interval_ns = 0;
  PJ::sdk::ArrayLimit array_limit;
};

/// Message parser scaffold for self-describing Arrow IPC streams.
class ArrowParser : public PJ::MessageParserPluginBase {
 public:
  /// Bind a self-describing IPC stream type and register its non-canonical schema classification.
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema_bytes) override {
    if (auto status = MessageParserPluginBase::bindSchema(type_name, schema_bytes); !status) {
      return status;
    }
    registerSchemaHandler(
        type_name, PJ::sdk::SchemaHandler{
                       .object_type = PJ::sdk::BuiltinObjectType::kNone,
                       .parse_scalars = nullptr,
                       .parse_object = nullptr,
                   });
    return PJ::okStatus();
  }

  /// Load parser options from the JSON configuration contract.
  PJ::Status loadConfig(std::string_view config_json) override {
    const auto parsed = nlohmann::json::parse(config_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      return PJ::unexpected("parser_arrow: malformed configuration JSON");
    }

    try {
      ArrowParserConfig loaded;
      loaded.timestamp_column = parsed.value("timestamp_column", std::string{});
      loaded.flatten_structs = parsed.value("flatten_structs", true);
      loaded.synthetic_interval_ns = parsed.value("synthetic_interval_ns", int64_t{0});
      loaded.array_limit = PJ::sdk::arrayLimitFromJson(parsed);
      config_ = std::move(loaded);
    } catch (const nlohmann::json::exception& error) {
      return PJ::unexpected(std::string("parser_arrow: invalid configuration: ") + error.what());
    }
    return PJ::okStatus();
  }

  /// Serialize all parser options to the JSON configuration contract.
  [[nodiscard]] std::string saveConfig() const override {
    nlohmann::json saved{
        {"timestamp_column", config_.timestamp_column},
        {"flatten_structs", config_.flatten_structs},
        {"synthetic_interval_ns", config_.synthetic_interval_ns},
    };
    PJ::sdk::arrayLimitToJson(saved, config_.array_limit);
    return saved.dump();
  }

  /// Decode, shape, diagnose, and bulk-ingest one self-describing Arrow IPC stream.
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    auto decoded = pj::parser_arrow::decodeIpcStream(payload);
    if (!decoded) {
      return PJ::unexpected(std::move(decoded).error());
    }

    auto shaped = pj::parser_arrow::shapeStream(
        std::move(*decoded), pj::parser_arrow::ShapeOptions{
                                 .timestamp_column = config_.timestamp_column,
                                 .flatten_structs = config_.flatten_structs,
                                 .message_timestamp_ns = timestamp_ns,
                                 .synthetic_interval_ns = config_.synthetic_interval_ns,
                                 .array_limit = config_.array_limit,
                             });
    if (!shaped) {
      return PJ::unexpected(std::move(shaped).error());
    }

    reportDroppedColumnsIfChanged(parserRuntimeHost(), shaped->dropped_columns, last_dropped_columns_);
    return writeHost().appendArrowStream(std::move(shaped->stream), shaped->timestamp_column);
  }

 private:
  ArrowParserConfig config_;
  std::vector<DroppedColumn> last_dropped_columns_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ArrowParser, kArrowManifest)
