#include <pj_base/sdk/data_source_patterns.hpp>

#include "parquet_dialog.hpp"
#include "parquet_helpers.hpp"
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

using PJ::ParquetHelpers::arrowTypeToPrimitive;
using PJ::ParquetHelpers::basenameWithoutExt;
using PJ::ParquetHelpers::findTimestampColumn;
using PJ::ParquetHelpers::getArrowValueRef;
using PJ::ParquetHelpers::getTimestampNanos;
using PJ::ParquetHelpers::isSupportedArrowType;

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
