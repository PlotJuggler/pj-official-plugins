#include <pj_base/sdk/data_source_patterns.hpp>

#include "parquet_dialog.hpp"
#include "parquet_manifest.hpp"

#include <nlohmann/json.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Extract the file basename (without extension) from a path.
/// Handles both '/' and '\\' separators.
std::string basenameWithoutExt(const std::string& filepath) {
  auto slash = filepath.find_last_of("/\\");
  auto start = (slash != std::string::npos) ? slash + 1 : 0;
  auto dot = filepath.rfind('.');
  if (dot != std::string::npos && dot > start) {
    return filepath.substr(start, dot - start);
  }
  return filepath.substr(start);
}

bool isSupportedArrowType(arrow::Type::type t) {
  return t == arrow::Type::BOOL || t == arrow::Type::INT8 || t == arrow::Type::INT16 ||
         t == arrow::Type::INT32 || t == arrow::Type::INT64 || t == arrow::Type::UINT8 ||
         t == arrow::Type::UINT16 || t == arrow::Type::UINT32 ||
         t == arrow::Type::UINT64 || t == arrow::Type::FLOAT ||
         t == arrow::Type::DOUBLE || t == arrow::Type::TIMESTAMP ||
         t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
}

/// Map Arrow type to PJ::PrimitiveType for field pre-registration.
PJ::PrimitiveType arrowTypeToPrimitive(arrow::Type::type t) {
  switch (t) {
    case arrow::Type::BOOL: return PJ::PrimitiveType::kBool;
    case arrow::Type::INT8: return PJ::PrimitiveType::kInt8;
    case arrow::Type::INT16: return PJ::PrimitiveType::kInt16;
    case arrow::Type::INT32: return PJ::PrimitiveType::kInt32;
    case arrow::Type::INT64: return PJ::PrimitiveType::kInt64;
    case arrow::Type::UINT8: return PJ::PrimitiveType::kUint8;
    case arrow::Type::UINT16: return PJ::PrimitiveType::kUint16;
    case arrow::Type::UINT32: return PJ::PrimitiveType::kUint32;
    case arrow::Type::UINT64: return PJ::PrimitiveType::kUint64;
    case arrow::Type::FLOAT: return PJ::PrimitiveType::kFloat32;
    case arrow::Type::DOUBLE: return PJ::PrimitiveType::kFloat64;
    case arrow::Type::TIMESTAMP: return PJ::PrimitiveType::kInt64;  // nanoseconds
    case arrow::Type::STRING: return PJ::PrimitiveType::kString;
    case arrow::Type::LARGE_STRING: return PJ::PrimitiveType::kString;
    default: return PJ::PrimitiveType::kFloat64;
  }
}

/// Extract a native-typed ValueRef from an Arrow array cell.
/// Returns NullValue for nulls and unsupported types.
PJ::sdk::ValueRef getArrowValueRef(const std::shared_ptr<arrow::Array>& array,
                                   int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) return PJ::NullValue{};

  switch (arrow_type) {
    case arrow::Type::BOOL:
      return std::static_pointer_cast<arrow::BooleanArray>(array)->Value(index);
    case arrow::Type::INT8:
      return std::static_pointer_cast<arrow::Int8Array>(array)->Value(index);
    case arrow::Type::INT16:
      return std::static_pointer_cast<arrow::Int16Array>(array)->Value(index);
    case arrow::Type::INT32:
      return std::static_pointer_cast<arrow::Int32Array>(array)->Value(index);
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(array)->Value(index);
    case arrow::Type::UINT8:
      return std::static_pointer_cast<arrow::UInt8Array>(array)->Value(index);
    case arrow::Type::UINT16:
      return std::static_pointer_cast<arrow::UInt16Array>(array)->Value(index);
    case arrow::Type::UINT32:
      return std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index);
    case arrow::Type::UINT64:
      return std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index);
    case arrow::Type::FLOAT:
      return std::static_pointer_cast<arrow::FloatArray>(array)->Value(index);
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index);
    case arrow::Type::TIMESTAMP: {
      auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_array->type());
      int64_t value = ts_array->Value(index);
      // Convert to nanoseconds
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: value *= 1000000000LL; break;
        case arrow::TimeUnit::MILLI: value *= 1000000LL; break;
        case arrow::TimeUnit::MICRO: value *= 1000LL; break;
        case arrow::TimeUnit::NANO: break;
      }
      // Arrow TIMESTAMP with timezone is already stored as UTC.
      // No offset adjustment needed.
      return value;
    }
    case arrow::Type::STRING: {
      auto str_array = std::static_pointer_cast<arrow::StringArray>(array);
      auto sv = str_array->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    case arrow::Type::LARGE_STRING: {
      auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(array);
      auto sv = str_array->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    default:
      return PJ::NullValue{};
  }
}

/// Extract timestamp in nanoseconds from an Arrow array cell (for the time axis).
int64_t getTimestampNanos(const std::shared_ptr<arrow::Array>& array,
                          int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) return 0;

  switch (arrow_type) {
    case arrow::Type::TIMESTAMP: {
      auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_array->type());
      int64_t value = ts_array->Value(index);
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: return value * 1000000000LL;
        case arrow::TimeUnit::MILLI: return value * 1000000LL;
        case arrow::TimeUnit::MICRO: return value * 1000LL;
        case arrow::TimeUnit::NANO: return value;
      }
      return value;
    }
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(array)->Value(index);
    case arrow::Type::UINT64:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index));
    case arrow::Type::DOUBLE: {
      double v = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index);
      return static_cast<int64_t>(v * 1e9);
    }
    case arrow::Type::FLOAT: {
      float v = std::static_pointer_cast<arrow::FloatArray>(array)->Value(index);
      return static_cast<int64_t>(static_cast<double>(v) * 1e9);
    }
    case arrow::Type::INT32:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::Int32Array>(array)->Value(index));
    case arrow::Type::UINT32:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index));
    default:
      return 0;
  }
}

// Heuristic: find a column that looks like a timestamp by name or type.
int findTimestampColumn(const std::shared_ptr<arrow::Schema>& schema) {
  static const std::vector<std::string> kTimestampNames = {
      "timestamp", "time", "t", "ts", "time_stamp", "datetime", "date_time",
      "_timestamp", "_time"};

  // Prefer Arrow TIMESTAMP typed columns
  for (int i = 0; i < schema->num_fields(); i++) {
    if (schema->field(i)->type()->id() == arrow::Type::TIMESTAMP) {
      return i;
    }
  }

  // Fallback: match by name (case-insensitive)
  for (int i = 0; i < schema->num_fields(); i++) {
    std::string name = schema->field(i)->name();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& candidate : kTimestampNames) {
      if (name == candidate) return i;
    }
  }

  return -1;
}

struct ColumnInfo {
  std::string name;
  arrow::Type::type arrow_type;
  int column_index;
};

class ParquetSource : public PJ::FileSourceBase {
 public:
  void* dialogContext() override { return &dialog_; }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    // Extract configuration from dialog state
    std::string filepath;
    {
      auto cfg_str = dialog_.saveConfig();
      auto cfg = nlohmann::json::parse(cfg_str, nullptr, false);
      if (cfg.is_discarded()) return PJ::unexpected(std::string("invalid config"));
      filepath = cfg.value("filepath", std::string{});
    }

    if (filepath.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    auto infile_result = arrow::io::ReadableFile::Open(filepath);
    if (!infile_result.ok()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath);
    }
    auto infile = *infile_result;

    auto reader_result =
        parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) {
      return PJ::unexpected(std::string("failed to open Parquet: ") +
                            reader_result.status().ToString());
    }
    auto reader = std::move(*reader_result);

    std::shared_ptr<arrow::Schema> schema;
    auto status = reader->GetSchema(&schema);
    if (!status.ok()) {
      return PJ::unexpected(std::string("failed to read schema"));
    }

    // Determine time column: dialog selection takes priority, then heuristic
    int ts_col = dialog_.timeColumnIndex();
    if (ts_col < 0) {
      ts_col = findTimestampColumn(schema);
    }
    auto ts_arrow_type = ts_col >= 0 ? schema->field(ts_col)->type()->id() : arrow::Type::NA;

    // Collect numeric data columns (excluding time column)
    std::vector<ColumnInfo> columns;
    for (int i = 0; i < schema->num_fields(); i++) {
      if (i == ts_col) continue;
      const auto& field = schema->field(i);
      if (isSupportedArrowType(field->type()->id())) {
        columns.push_back(ColumnInfo{field->name(), field->type()->id(), i});
      }
    }

    if (columns.empty()) {
      return PJ::unexpected(std::string("no supported columns found"));
    }

    // Create a SINGLE topic from the file basename
    std::string topic_name = basenameWithoutExt(filepath);
    if (topic_name.empty()) topic_name = "data";

    auto topic = writeHost().ensureTopic(topic_name);
    if (!topic) return PJ::unexpected(topic.error());

    // Pre-register ALL numeric columns before writing any data
    for (const auto& col : columns) {
      auto field = writeHost().ensureField(*topic, col.name, arrowTypeToPrimitive(col.arrow_type));
      if (!field) return PJ::unexpected(field.error());
    }

    auto metadata = reader->parquet_reader()->metadata();
    int64_t total_rows = metadata->num_rows();
    (void)runtimeHost().progressStart(
        "Importing Parquet", static_cast<uint64_t>(total_rows), true);

    // Read via RecordBatchReader
    std::shared_ptr<arrow::RecordBatchReader> batch_reader;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    status = reader->GetRecordBatchReader(&batch_reader);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (!status.ok()) {
      return PJ::unexpected(std::string("failed to create batch reader"));
    }

    int64_t rows_processed = 0;
    std::shared_ptr<arrow::RecordBatch> batch;

    // Pre-allocate row fields vector (reused per row)
    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    row_fields.reserve(columns.size());

    while (batch_reader->ReadNext(&batch).ok() && batch) {
      int64_t batch_rows = batch->num_rows();

      // Cache column arrays for this batch
      std::vector<std::shared_ptr<arrow::Array>> col_arrays;
      col_arrays.reserve(columns.size());
      for (const auto& col : columns) {
        col_arrays.push_back(batch->column(col.column_index));
      }

      std::shared_ptr<arrow::Array> ts_array;
      if (ts_col >= 0) {
        ts_array = batch->column(ts_col);
      }

      // Build per-row timestamps, then sort by timestamp to ensure monotonic order
      // (matches original PlotJuggler behavior)
      std::vector<std::pair<int64_t, int64_t>> ts_row_pairs;  // (timestamp_ns, row_index)
      ts_row_pairs.reserve(static_cast<size_t>(batch_rows));
      for (int64_t row = 0; row < batch_rows; row++) {
        int64_t ts_ns = 0;
        if (ts_col >= 0) {
          ts_ns = getTimestampNanos(ts_array, row, ts_arrow_type);
        } else {
          ts_ns = (rows_processed + row) * 1000000000LL;
        }
        ts_row_pairs.push_back({ts_ns, row});
      }
      std::sort(ts_row_pairs.begin(), ts_row_pairs.end());

      // Iterate in sorted order, building a multi-field record per row
      for (const auto& [ts_ns, row] : ts_row_pairs) {
        row_fields.clear();

        for (size_t c = 0; c < columns.size(); c++) {
          auto val = getArrowValueRef(col_arrays[c], row, columns[c].arrow_type);
          // Skip null cells — finishRow() auto-fills with null
          if (PJ::sdk::isNull(val)) continue;

          row_fields.push_back({
              .name = columns[c].name,
              .value = val,
          });
        }

        if (!row_fields.empty()) {
          auto write_status = writeHost().appendRecord(
              *topic, PJ::Timestamp{ts_ns},
              PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
          if (!write_status) return write_status;
        }
      }

      rows_processed += batch_rows;

      if (runtimeHost().isStopRequested()) {
        return PJ::unexpected(std::string("import cancelled"));
      }
      (void)runtimeHost().progressUpdate(static_cast<uint64_t>(rows_processed));
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "Imported " + std::to_string(rows_processed) + " rows, " +
            std::to_string(columns.size()) + " series");

    return PJ::okStatus();
  }

 private:
  ParquetDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(ParquetSource, kParquetManifest)

PJ_DIALOG_PLUGIN(ParquetDialog)
