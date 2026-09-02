#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arrow_error.hpp"
#include "arrow_manifest.hpp"
#include "ipc_decoder.hpp"
#include "table_shaper.hpp"

namespace {

using pj::parser_arrow::DroppedColumn;
using pj::parser_arrow::ShapeWarning;

/// One diagnostic plus its aggregated occurrence count, which is intentionally excluded from the dedup key.
struct PendingDiagnostic {
  ShapeWarning warning;
  uint64_t occurrences = 1;
};

/// Return whether two diagnostic lists contain identical code/message pairs in the same order.
[[nodiscard]] bool sameDiagnostics(
    const std::vector<PendingDiagnostic>& current, const std::vector<ShapeWarning>& previous) noexcept {
  if (current.size() != previous.size()) {
    return false;
  }
  return std::equal(
      current.begin(), current.end(), previous.begin(),
      [](const PendingDiagnostic& current_diagnostic, const ShapeWarning& previous_warning) {
        return current_diagnostic.warning.code == previous_warning.code &&
               current_diagnostic.warning.message == previous_warning.message;
      });
}

/// Empty-width variable lists carry a format sentinel so their planning reason stays distinct from host rejection.
[[nodiscard]] bool wasEmptyInFirstBatch(const DroppedColumn& column) noexcept {
  constexpr std::string_view marker = "(empty)";
  return column.format.size() >= marker.size() && column.format.ends_with(marker);
}

/// Build the bounded human-readable warning for Arrow columns removed before host ingest.
[[nodiscard]] std::string droppedColumnsMessage(const std::vector<DroppedColumn>& columns) {
  bool has_unsupported = false;
  bool has_empty_first_batch = false;
  for (const auto& column : columns) {
    if (wasEmptyInFirstBatch(column)) {
      has_empty_first_batch = true;
    } else {
      has_unsupported = true;
    }
  }

  std::string message = std::to_string(columns.size());
  if (!has_empty_first_batch) {
    message += " column(s) removed from the Arrow stream (unsupported host type): ";
    message += pj::parser_arrow::formatDroppedColumns(columns, pj::parser_arrow::kMaxDroppedColumnsListed);
    return message;
  }
  if (!has_unsupported) {
    message += " column(s) removed from the Arrow stream (empty in first batch): ";
    message += pj::parser_arrow::formatDroppedColumns(columns, pj::parser_arrow::kMaxDroppedColumnsListed);
    return message;
  }

  message += " column(s) removed from the Arrow stream: ";
  const std::size_t listed = std::min(columns.size(), pj::parser_arrow::kMaxDroppedColumnsListed);
  for (std::size_t index = 0; index < listed; ++index) {
    if (index != 0) {
      message += ", ";
    }
    const auto& column = columns[index];
    message += column.name + ":" + column.format;
    message += wasEmptyInFirstBatch(column) ? " (empty in first batch)" : " (unsupported host type)";
  }
  if (listed < columns.size()) {
    message += ", …";
  }
  return message;
}

/// Report a changed ordered diagnostic set without making allocation or host-reporting failures fatal to parsing.
void reportDiagnosticsIfChanged(
    const PJ::sdk::ParserRuntimeHostView& runtime_host, const pj::parser_arrow::ShapedStream& shaped,
    std::vector<ShapeWarning>& last_diagnostics) noexcept {
  try {
    std::vector<PendingDiagnostic> diagnostics;
    diagnostics.reserve(shaped.warnings.size() + 3);
    for (const auto& warning : shaped.warnings) {
      diagnostics.push_back(PendingDiagnostic{warning});
    }
    if (!shaped.dropped_columns.empty()) {
      diagnostics.push_back(
          PendingDiagnostic{ShapeWarning{
              .code = "parser_arrow.dropped_columns", .message = droppedColumnsMessage(shaped.dropped_columns)}});
    }
    if (shaped.runtime->rows_truncated > 0) {
      diagnostics.push_back(
          PendingDiagnostic{
              ShapeWarning{
                  .code = "parser_arrow.truncated_lists",
                  .message = "Arrow list rows were truncated to the planned width; first affected column '" +
                             shaped.runtime->first_truncated_column + "'"},
              static_cast<uint64_t>(shaped.runtime->rows_truncated)});
    }
    if (shaped.runtime->float_axis_magnitude_exceeded) {
      diagnostics.push_back(
          PendingDiagnostic{ShapeWarning{
              .code = "parser_arrow.float_axis_precision",
              .message = "float32 timestamp axis '" + shaped.runtime->float_axis_column +
                         "' reached |seconds| >= 2^23; float32 spacing is at least one second at this magnitude"}});
    }

    if (sameDiagnostics(diagnostics, last_diagnostics)) {
      return;
    }

    std::vector<ShapeWarning> next_diagnostics;
    next_diagnostics.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
      next_diagnostics.push_back(diagnostic.warning);
    }
    for (const auto& diagnostic : diagnostics) {
      runtime_host.reportDiagnostic(
          PJ::sdk::ParserDiagnosticLevel::Warning, diagnostic.warning.code, diagnostic.warning.message,
          diagnostic.occurrences);
    }
    last_diagnostics.swap(next_diagnostics);
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
      return PJ::unexpected(pj::parser_arrow::parserError("malformed configuration JSON"));
    }

    try {
      ArrowParserConfig loaded;
      loaded.timestamp_column = parsed.value("timestamp_column", std::string{});
      loaded.flatten_structs = parsed.value("flatten_structs", true);
      loaded.synthetic_interval_ns = parsed.value("synthetic_interval_ns", int64_t{0});
      loaded.array_limit = PJ::sdk::arrayLimitFromJson(parsed);
      config_ = std::move(loaded);
    } catch (const nlohmann::json::exception& error) {
      return PJ::unexpected(pj::parser_arrow::parserError(std::string("invalid configuration: ") + error.what()));
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

    auto status = writeHost().appendArrowStream(std::move(shaped->stream), shaped->timestamp_column);
    reportDiagnosticsIfChanged(parserRuntimeHost(), *shaped, last_diagnostics_);
    return status;
  }

 private:
  ArrowParserConfig config_;
  std::vector<ShapeWarning> last_diagnostics_;
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(ArrowParser, kArrowManifest)
