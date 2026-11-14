#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <vector>

#include "../common/localization/display_language.hpp"
#include "csv_dialog.hpp"
#include "csv_manifest.hpp"
#include "csv_parser.hpp"

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

class CsvSource : public PJ::FileSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

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
      if (cfg.is_discarded()) {
        return PJ::unexpected(std::string("invalid config"));
      }
      filepath = cfg.value("filepath", std::string{});
    }

    if (filepath.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath);
    }

    // Build parse config from dialog state
    PJ::CSV::CsvParseConfig config;
    config.delimiter = dialog_.delimiter();
    config.time_column_index = dialog_.timeColumnIndex();
    config.combined_column_index = dialog_.combinedColumnIndex();
    config.combined_columns.assign(dialog_.combinedPairs().begin(), dialog_.combinedPairs().end());
    if (dialog_.useCustomFormat()) {
      config.custom_time_format = dialog_.customTimeFormat();
    }

    // Count lines for progress
    int total_lines = 0;
    {
      std::string tmp;
      while (std::getline(file, tmp)) {
        total_lines++;
      }
      file.clear();
      file.seekg(0);
    }
    config.total_lines = total_lines;

    (void)runtimeHost().progressStart(
        pj::localization::text("Importing CSV", "正在导入 CSV", "正在匯入 CSV"), static_cast<uint64_t>(total_lines),
        true);

    auto result = PJ::CSV::ParseCsvData(file, config, [this](int current, int /*total*/) -> bool {
      if (runtimeHost().isStopRequested()) {
        return false;
      }
      return runtimeHost().progressUpdate(static_cast<uint64_t>(current));
    });

    // ParseCsvData sets success=false either on a real parse error OR when the user
    // stopped the load (the progress callback returned false). In the stop case the
    // rows parsed so far are already in `result` — write them so the host's
    // "Stop and Keep" can flush them (Discard evicts the dataset host-side anyway).
    // Only a genuine failure (not a stop) aborts with no data.
    const bool stopped_by_user = runtimeHost().isStopRequested();
    if (!result.success && !stopped_by_user) {
      return PJ::unexpected(std::string("CSV parsing failed"));
    }

    // Interactive "continue?" prompts only make sense on a normal load. When the user
    // already chose to stop, don't ask more questions — just keep what was parsed.
    if (!stopped_by_user) {
      // --- Non-monotonic time warning ---
      if (result.time_is_non_monotonic) {
        if (!runtimeHost().askContinue(
                pj::localization::text("Non-monotonic Timestamps", "时间戳非单调递增", "時間戳記非單調遞增"),
                pj::localization::text(
                    "The time column is not monotonically increasing.\n"
                    "Data will be sorted automatically after loading.\n\nContinue?",
                    "时间列不是单调递增的。\n数据将在加载后自动排序。\n\n是否继续？",
                    "時間欄不是單調遞增的。\n資料將在載入後自動排序。\n\n是否繼續？"))) {
          return PJ::unexpected(std::string("Loading aborted by user"));
        }
      }

      // --- Skipped lines warning with detail ---
      std::string detail;
      for (const auto& warn : result.warnings) {
        if (warn.type == PJ::CSV::CsvParseWarning::WRONG_COLUMN_COUNT ||
            warn.type == PJ::CSV::CsvParseWarning::INVALID_TIMESTAMP) {
          detail += pj::localization::text("Line ", "行 ", "行 ") + std::to_string(warn.line_number) + ": " +
                    (warn.localized_detail.empty() ? warn.detail : warn.localized_detail) + "\n";
        }
      }
      if (!detail.empty()) {
        if (!runtimeHost().askContinue(
                pj::localization::text("Rows Skipped", "跳过行", "略過資料列"),
                pj::localization::text(
                    "Some rows have an incorrect number of columns and will be skipped:\n\n",
                    "部分行的列数不正确，将被跳过：\n\n", "部分資料列的欄數不正確，將被略過：\n\n") +
                    detail + pj::localization::text("\nContinue loading?", "\n是否继续加载？", "\n是否繼續載入？"))) {
          return PJ::unexpected(std::string("Loading aborted by user"));
        }
      }
    }

    // --- Classify columns ---
    std::string topic_name = basenameWithoutExt(filepath);
    if (topic_name.empty()) {
      topic_name = "data";
    }

    auto topic = writeHost().ensureTopic(topic_name);
    if (!topic) {
      return PJ::unexpected(topic.error());
    }

    // Identify which columns are data (not time, not combined components)
    std::vector<size_t> numeric_col_indices;
    std::vector<size_t> string_col_indices;

    for (size_t i = 0; i < result.columns.size(); i++) {
      // Skip time column
      if (config.time_column_index >= 0 && static_cast<size_t>(config.time_column_index) == i) {
        continue;
      }
      // Skip combined date/time component columns
      if (result.combined_component_indices.count(static_cast<int>(i)) > 0) {
        continue;
      }

      const auto& col = result.columns[i];
      bool has_numeric = !col.numeric_points.empty();
      bool has_string = !col.string_points.empty();

      if (has_numeric) {
        numeric_col_indices.push_back(i);
        if (has_string) {
          runtimeHost().showWarning(
              pj::localization::text("Mixed Column Type", "混合列类型", "混合欄位類型"),
              pj::localization::text("Column '", "列 '", "欄 '") + col.name +
                  pj::localization::text("' has ", "' 中有 ", "' 中有 ") + std::to_string(col.string_points.size()) +
                  pj::localization::text(
                      " non-numeric cells that were skipped.", " 个非数值单元格已跳过。", " 個非數值儲存格已略過。"));
        }
      } else if (has_string) {
        string_col_indices.push_back(i);
      }
      // Columns with no data at all: register as numeric (empty series)
      // This matches old behavior: plot_data.addNumeric(name) for empty cols
      if (!has_numeric && !has_string) {
        numeric_col_indices.push_back(i);
      }
    }

    // --- Pre-register ALL columns before writing any data ---
    // Rows are appended by handle: the by-name path copies every column name
    // into each row and makes the host re-resolve it, per cell.
    std::vector<PJ::sdk::FieldHandle> column_handles(result.columns.size());
    for (size_t col_idx : numeric_col_indices) {
      auto field = writeHost().ensureField(*topic, result.columns[col_idx].name, PJ::PrimitiveType::kFloat64);
      if (!field) {
        return PJ::unexpected(field.error());
      }
      column_handles[col_idx] = *field;
    }
    for (size_t col_idx : string_col_indices) {
      auto field = writeHost().ensureField(*topic, result.columns[col_idx].name, PJ::PrimitiveType::kString);
      if (!field) {
        return PJ::unexpected(field.error());
      }
      column_handles[col_idx] = *field;
    }

    // --- Write one record per timestamp, straight from the parsed columns ---
    std::vector<PJ::sdk::BoundFieldValue> row_fields;
    PJ::Status write_status = PJ::okStatus();
    PJ::CSV::ForEachRecord(
        result.columns, numeric_col_indices, string_col_indices,
        [&](int64_t ts, const std::vector<PJ::CSV::RecordCell>& cells) {
          row_fields.clear();
          for (const auto& cell : cells) {
            if (cell.text != nullptr) {
              row_fields.push_back({.field = column_handles[cell.column], .value = std::string_view(*cell.text)});
            } else {
              row_fields.push_back({.field = column_handles[cell.column], .value = *cell.number});
            }
          }
          write_status = writeHost().appendBoundRecord(
              *topic, PJ::Timestamp{ts},
              PJ::Span<const PJ::sdk::BoundFieldValue>(row_fields.data(), row_fields.size()));
          return write_status.has_value();
        });
    if (!write_status) {
      return write_status;
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        pj::localization::text("Imported ", "已导入 ", "已匯入 ") + std::to_string(result.lines_processed) +
            pj::localization::text(" rows, ", " 行，跳过 ", " 列，略過 ") + std::to_string(result.lines_skipped) +
            pj::localization::text(" skipped", " 行", " 列"));

    return PJ::okStatus();
  }

 private:
  CsvDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(CsvSource, kCsvManifest)

PJ_DIALOG_PLUGIN(CsvDialog, kCsvManifest)
