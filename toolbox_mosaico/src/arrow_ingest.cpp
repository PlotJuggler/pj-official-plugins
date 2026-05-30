// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ingest.hpp"

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_primitive.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "image_metadata.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"

namespace mosaico {

namespace {

// --------------------------------------------------------------------------
// Per-row Arrow column readers. Each dispatches on the chunk's runtime type so
// the same helper handles the int32/int64, BINARY/STRING and — critically for
// the Mosaico server — the BINARY_VIEW / STRING_VIEW variants Arrow >=15 emits
// for variable-length columns. Reading those view types with a plain
// BinaryArray/StringArray cast returns nothing, which is the root cause of
// image topics "barely downloading" (empty encoding/data -> the topic errored).
// --------------------------------------------------------------------------

[[nodiscard]] std::int32_t arrowI32At(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return 0;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return 0;
      }
      switch (chunk->type_id()) {
        case arrow::Type::INT32:
          return std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT32:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::UInt32Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT64:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(chunk_row));
        case arrow::Type::UINT64:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::UInt64Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT16:
          return std::static_pointer_cast<arrow::Int16Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT16:
          return std::static_pointer_cast<arrow::UInt16Array>(chunk)->Value(chunk_row);
        default:
          return 0;
      }
    }
    chunk_row -= chunk->length();
  }
  return 0;
}

[[nodiscard]] std::int64_t arrowI64At(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return 0;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return 0;
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
          return 0;
      }
    }
    chunk_row -= chunk->length();
  }
  return 0;
}

// Read a UTF-8 string handling STRING / LARGE_STRING / STRING_VIEW.
[[nodiscard]] std::string arrowStringAt(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return {};
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return {};
      }
      switch (chunk->type_id()) {
        case arrow::Type::STRING:
          return std::static_pointer_cast<arrow::StringArray>(chunk)->GetString(chunk_row);
        case arrow::Type::LARGE_STRING:
          return std::static_pointer_cast<arrow::LargeStringArray>(chunk)->GetString(chunk_row);
        case arrow::Type::STRING_VIEW:
          // Arrow >=15 emits Utf8View for variable-length string columns; the
          // Mosaico server uses it for `encoding`/`format`/`frame_id`.
          return std::static_pointer_cast<arrow::StringViewArray>(chunk)->GetString(chunk_row);
        default:
          return {};
      }
    }
    chunk_row -= chunk->length();
  }
  return {};
}

// Read a bool handling BOOL; returns @p fallback on null/missing/other type.
[[nodiscard]] bool arrowBoolAt(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row, bool fallback) {
  if (!col || row < 0 || row >= col->length()) {
    return fallback;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return fallback;
      }
      if (chunk->type_id() == arrow::Type::BOOL) {
        return std::static_pointer_cast<arrow::BooleanArray>(chunk)->Value(chunk_row);
      }
      return fallback;
    }
    chunk_row -= chunk->length();
  }
  return fallback;
}

// View a row's bytes as a span, handling BINARY / LARGE_BINARY /
// FIXED_SIZE_BINARY / BINARY_VIEW. The span borrows the Arrow buffer; the
// caller must not retain it past the lifetime of @p col.
[[nodiscard]] PJ::Span<const std::uint8_t> arrowBinaryAt(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return {};
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return {};
      }
      switch (chunk->type_id()) {
        case arrow::Type::BINARY: {
          auto bin = std::static_pointer_cast<arrow::BinaryArray>(chunk);
          int32_t length = 0;
          const std::uint8_t* ptr = bin->GetValue(chunk_row, &length);
          return PJ::Span<const std::uint8_t>(ptr, static_cast<std::size_t>(length));
        }
        case arrow::Type::LARGE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::LargeBinaryArray>(chunk);
          int64_t length = 0;
          const std::uint8_t* ptr = bin->GetValue(chunk_row, &length);
          return PJ::Span<const std::uint8_t>(ptr, static_cast<std::size_t>(length));
        }
        case arrow::Type::FIXED_SIZE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(chunk);
          return PJ::Span<const std::uint8_t>(bin->GetValue(chunk_row), static_cast<std::size_t>(bin->byte_width()));
        }
        case arrow::Type::BINARY_VIEW: {
          // Arrow >=15 emits BinaryView for variable-length binary columns;
          // the Mosaico server uses it for the image `data` column.
          auto bin = std::static_pointer_cast<arrow::BinaryViewArray>(chunk);
          const std::string_view view = bin->GetView(chunk_row);
          return PJ::Span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(view.data()), view.size());
        }
        default:
          return {};
      }
    }
    chunk_row -= chunk->length();
  }
  return {};
}

// Return the first column present under any of @p names, else nullptr.
[[nodiscard]] std::shared_ptr<arrow::ChunkedArray> firstPresentColumn(
    const std::shared_ptr<arrow::Table>& table, std::initializer_list<const char*> names) {
  for (const char* name : names) {
    if (auto col = table->GetColumnByName(name)) {
      return col;
    }
  }
  return nullptr;
}

}  // namespace

std::string detectTimestampColumn(const ArrowSchema* schema) {
  if (schema == nullptr || schema->children == nullptr) {
    return {};
  }
  // First pass: Arrow TIMESTAMP type. The format string for timestamps
  // starts with "ts" per Arrow C ABI spec (e.g. "tsn:UTC").
  for (int64_t i = 0; i < schema->n_children; ++i) {
    const auto* child = schema->children[i];
    if (child != nullptr && child->format != nullptr) {
      std::string_view fmt(child->format);
      if (fmt.size() >= 2 && fmt[0] == 't' && fmt[1] == 's') {
        return child->name != nullptr ? std::string(child->name) : std::string();
      }
    }
  }
  // Second pass: name heuristics. Order matters — most-specific first.
  static const std::array<std::string_view, 5> kNames = {
      "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (std::string_view preferred : kNames) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
      const auto* child = schema->children[i];
      if (child != nullptr && child->name != nullptr && std::string_view(child->name) == preferred) {
        return std::string(child->name);
      }
    }
  }
  return {};
}

arrow::Result<std::shared_ptr<arrow::Table>> normalizeViewColumns(std::shared_ptr<arrow::Table> table) {
  if (!table) {
    return arrow::Status::Invalid("normalizeViewColumns: null table");
  }
  // Cast Arrow "view" string/binary columns (Utf8View / BinaryView, which the
  // Mosaico server / Arrow >= 15 emit for e.g. frame_id) to canonical Utf8 /
  // Binary. pj_datastore's nanoarrow import maps only STRING / LARGE_STRING; a
  // view-typed column anywhere in the record batch corrupts the import for the
  // WHOLE batch, so every column (even plain doubles) lands as null. Applied to
  // the scalar (appendArrowStream) pipeline only — the image path reads view
  // types directly.
  std::vector<std::shared_ptr<arrow::ChunkedArray>> cols = table->columns();
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(static_cast<std::size_t>(table->num_columns()));
  bool changed = false;
  for (int i = 0; i < table->num_columns(); ++i) {
    const auto& field = table->schema()->field(i);
    std::shared_ptr<arrow::DataType> target;
    if (field->type()->id() == arrow::Type::STRING_VIEW) {
      target = arrow::utf8();
    } else if (field->type()->id() == arrow::Type::BINARY_VIEW) {
      target = arrow::binary();
    }
    if (target != nullptr) {
      ARROW_ASSIGN_OR_RAISE(auto casted, arrow::compute::Cast(arrow::Datum(cols[static_cast<std::size_t>(i)]), target));
      cols[static_cast<std::size_t>(i)] = casted.chunked_array();
      fields.push_back(field->WithType(target));
      changed = true;
    } else {
      fields.push_back(field);
    }
  }
  if (!changed) {
    return table;
  }
  return arrow::Table::Make(std::make_shared<arrow::Schema>(fields), cols, table->num_rows());
}

arrow::Result<std::shared_ptr<arrow::Table>> flattenStructColumns(std::shared_ptr<arrow::Table> table) {
  if (!table) {
    return arrow::Status::Invalid("flattenStructColumns: null table");
  }

  // Arrow's Table::Flatten only peels one struct layer at a time and uses
  // "." as the separator. Loop until there are no more struct columns, then
  // rewrite the dotted names with "/" so the resulting curve paths match
  // PJ3's flattenArray output (toolbox_mosaico.cpp:374).
  auto current = std::move(table);
  while (true) {
    bool has_struct = false;
    for (int i = 0; i < current->num_columns(); ++i) {
      if (current->schema()->field(i)->type()->id() == arrow::Type::STRUCT) {
        has_struct = true;
        break;
      }
    }
    if (!has_struct) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(current, current->Flatten());
  }

  // Rename "parent.child" → "parent/child" to keep curve keys consistent
  // with PJ3 (e.g. "<sequence>/<topic>/pose/position/x"). Only the fields
  // change — the data and chunk layout carry through untouched.
  std::vector<std::shared_ptr<arrow::Field>> renamed_fields;
  renamed_fields.reserve(static_cast<std::size_t>(current->num_columns()));
  bool needs_rename = false;
  for (int i = 0; i < current->num_columns(); ++i) {
    const auto& field = current->schema()->field(i);
    if (field->name().find('.') == std::string::npos) {
      renamed_fields.push_back(field);
      continue;
    }
    needs_rename = true;
    std::string new_name = field->name();
    std::replace(new_name.begin(), new_name.end(), '.', '/');
    renamed_fields.push_back(arrow::field(new_name, field->type(), field->nullable(), field->metadata()));
  }
  if (needs_rename) {
    auto renamed_schema = std::make_shared<arrow::Schema>(renamed_fields);
    current = arrow::Table::Make(renamed_schema, current->columns(), current->num_rows());
  }

  // Coalesce per-column chunks into one. Table::Flatten extracts struct
  // children as Arrays that reference the parent struct's buffers — when
  // those reach ExportRecordBatchReader, the C ABI batch carries the data
  // pointer at offset 0 but the conceptual data lives at the parent's
  // slice offset. The result downstream: pj_datastore's importArrowStream
  // reads from the wrong slot and every numeric column lands as zeros.
  // CombineChunks normalizes each column into a single contiguous chunk
  // with offset = 0, which exports cleanly.
  return current->CombineChunks();
}

PJ::Status pumpStreamToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, std::string_view topic_name,
    ArrowArrayStream* stream, std::string_view timestamp_col) {
  if (!host.valid()) {
    return PJ::unexpected("arrow_ingest: toolbox host not bound");
  }
  if (stream == nullptr) {
    return PJ::unexpected("arrow_ingest: null stream");
  }
  // The data source already carries the sequence name (it is created once per
  // Download in FetchWorker::datasetForFetch and shared by every topic), so the
  // topic is registered under its BARE name. The resulting catalog tree is
  // <sequence> ▸ <topic> ▸ fields.
  auto topic = host.ensureTopic(source, topic_name);
  if (!topic) {
    return PJ::unexpected(std::move(topic).error());
  }
  return host.appendArrowStream(*topic, stream, timestamp_col);
}

PJ::Expected<ImagePushOutcome> pushImageRowsToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, const std::string& topic_name,
    const std::shared_ptr<arrow::Table>& table, const std::string& ts_field, std::int64_t synth_anchor_ns,
    std::int64_t synth_interval_ns) {
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("image topic '") + topic_name + "': null table");
  }
  const auto data_col = table->GetColumnByName("data");
  if (!data_col) {
    return PJ::unexpected(std::string("image topic '") + topic_name + "' missing 'data' column");
  }
  // Resolve columns once; geometry/encoding are read per-row below.
  const auto width_col = table->GetColumnByName("width");
  const auto height_col = table->GetColumnByName("height");
  const auto stride_col = firstPresentColumn(table, {"stride", "step", "row_step"});
  const auto encoding_col = table->GetColumnByName("encoding");
  const auto format_col = table->GetColumnByName("format");
  const auto bigendian_col = table->GetColumnByName("is_bigendian");
  const auto ts_col = ts_field.empty() ? nullptr : table->GetColumnByName(ts_field);

  // Register the object topic ONCE under its BARE name on the shared data
  // source. The catalog tree is <sequence> ▸ <topic> ▸ image, grouped with the
  // scalar siblings.
  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalImageMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ImagePushOutcome outcome;
  const std::int64_t num_rows = table->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const auto bytes_span = arrowBinaryAt(data_col, row);
    if (bytes_span.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            std::string("image topic '") + topic_name + "' row " + std::to_string(row) + ": missing/empty 'data'";
      }
      continue;
    }

    const std::int32_t width = arrowI32At(width_col, row);
    const std::int32_t height = arrowI32At(height_col, row);
    const std::int32_t stride = arrowI32At(stride_col, row);
    std::string encoding = arrowStringAt(encoding_col, row);
    // Capture emptiness of the per-row `encoding` BEFORE the `format` fallback
    // overwrites it — a row that arrived with no pixel `encoding` is a
    // pure-compressed frame (geometry lives inside the blob). Reused below for
    // the is_compressed test so we don't re-materialize the string column.
    const bool encoding_was_empty = encoding.empty();
    const std::string format = arrowStringAt(format_col, row);
    // Pure-compressed topics ship only `format` (jpeg/png) with no pixel
    // `encoding`; fall back so the blob still carries a usable encoding.
    if (encoding.empty()) {
      encoding = format;
    }
    if (encoding.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("image topic '") + topic_name + "' row " + std::to_string(row) +
                              ": missing both 'encoding' and 'format'";
      }
      continue;
    }
    // A raw (non-compressed) frame needs positive geometry. A compressed frame
    // (encoding came from `format`, geometry lives inside the blob) is allowed
    // to have width/height == 0.
    const bool is_compressed = encoding_col == nullptr || encoding_was_empty;
    if (!is_compressed && (width <= 0 || height <= 0)) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("image topic '") + topic_name + "' row " + std::to_string(row) +
                              ": non-positive geometry (width=" + std::to_string(width) +
                              " height=" + std::to_string(height) + ")";
      }
      continue;
    }

    std::int64_t ts_ns = ts_col ? arrowI64At(ts_col, row) : (synth_anchor_ns + row * synth_interval_ns);

    PJ::sdk::Image img;
    img.width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
    img.height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
    img.row_step = stride > 0 ? static_cast<std::uint32_t>(stride) : 0U;
    img.encoding = std::move(encoding);
    // is_bigendian describes the PRODUCER's byte order and matters only for
    // multi-byte raw encodings (mono16). When the column is null/absent we
    // default to false (little-endian), matching the canonical sdk::Image
    // struct default (Image.hpp) and the ROS convention. An explicit column
    // value is always honored.
    img.is_bigendian = arrowBoolAt(bigendian_col, row, /*fallback=*/false);
    img.timestamp_ns = ts_ns;
    img.data = bytes_span;  // borrowed; serializeImage copies the bytes.

    const std::vector<std::uint8_t> blob = PJ::serializeImage(img);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    ++outcome.pushed;
  }
  return outcome;
}

}  // namespace mosaico
