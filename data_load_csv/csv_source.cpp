#include <pj_base/sdk/data_source_patterns.hpp>

#include "csv_dialog.hpp"
#include "csv_manifest.hpp"
#include "csv_parser.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <string>
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

class CsvSource : public PJ::FileSourceBase {
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

    std::ifstream file(filepath);
    if (!file.is_open()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath);
    }

    // Build parse config from dialog state
    PJ::CSV::CsvParseConfig config;
    config.delimiter = dialog_.delimiter();
    config.time_column_index = dialog_.timeColumnIndex();
    config.combined_column_index = dialog_.combinedColumnIndex();
    config.combined_columns.assign(dialog_.combinedPairs().begin(),
                                    dialog_.combinedPairs().end());
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
        "Importing CSV", static_cast<uint64_t>(total_lines), true);

    auto result = PJ::CSV::ParseCsvData(
        file, config, [this](int current, int /*total*/) -> bool {
          if (runtimeHost().isStopRequested()) return false;
          return runtimeHost().progressUpdate(static_cast<uint64_t>(current));
        });

    if (!result.success) {
      return PJ::unexpected(std::string("CSV parsing failed"));
    }

    // --- Non-monotonic time warning ---
    if (result.time_is_non_monotonic) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "Time column is not monotonically increasing. "
          "Data will be sorted by timestamp. This may indicate an issue with the input data.");
    }

    // --- Skipped lines warning with detail ---
    {
      std::string detail;
      for (const auto& warn : result.warnings) {
        if (warn.type == PJ::CSV::CsvParseWarning::WRONG_COLUMN_COUNT ||
            warn.type == PJ::CSV::CsvParseWarning::INVALID_TIMESTAMP) {
          detail += "Line " + std::to_string(warn.line_number) + ": " + warn.detail + "\n";
        }
      }
      if (!detail.empty()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "Some lines were skipped:\n" + detail);
      }
    }

    // --- Classify columns ---
    std::string topic_name = basenameWithoutExt(filepath);
    if (topic_name.empty()) topic_name = "data";

    auto topic = writeHost().ensureTopic(topic_name);
    if (!topic) return PJ::unexpected(topic.error());

    // Identify which columns are data (not time, not combined components)
    std::vector<size_t> numeric_col_indices;
    std::vector<size_t> string_col_indices;

    for (size_t i = 0; i < result.columns.size(); i++) {
      // Skip time column
      if (config.time_column_index >= 0 &&
          static_cast<size_t>(config.time_column_index) == i) {
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
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "Column '" + col.name + "' has " + std::to_string(col.string_points.size()) +
              " non-numeric cells that were skipped");
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
    for (size_t col_idx : numeric_col_indices) {
      auto field = writeHost().ensureField(
          *topic, result.columns[col_idx].name, PJ::PrimitiveType::kFloat64);
      if (!field) return PJ::unexpected(field.error());
    }
    for (size_t col_idx : string_col_indices) {
      auto field = writeHost().ensureField(
          *topic, result.columns[col_idx].name, PJ::PrimitiveType::kString);
      if (!field) return PJ::unexpected(field.error());
    }

    // --- Build merged timeline and write records ---
    // Each data point is (timestamp_ns, column_index, is_string, numeric_value, string_value).
    // We sort by timestamp and group all fields at the same timestamp into one record.
    struct DataPoint {
      int64_t ts_ns;
      size_t col_idx;
      bool is_string;
      double numeric_val;
      std::string string_val;
    };

    std::vector<DataPoint> all_points;
    for (size_t col_idx : numeric_col_indices) {
      const auto& col = result.columns[col_idx];
      for (const auto& [t, v] : col.numeric_points) {
        all_points.push_back(
            {static_cast<int64_t>(t * 1e9), col_idx, false, v, {}});
      }
    }
    for (size_t col_idx : string_col_indices) {
      const auto& col = result.columns[col_idx];
      for (const auto& [t, v] : col.string_points) {
        all_points.push_back(
            {static_cast<int64_t>(t * 1e9), col_idx, true, 0.0, v});
      }
    }
    std::stable_sort(all_points.begin(), all_points.end(),
                     [](const DataPoint& a, const DataPoint& b) {
                       return a.ts_ns < b.ts_ns;
                     });

    // Group by timestamp and write multi-field records
    std::vector<PJ::sdk::NamedFieldValue> row_fields;
    size_t i = 0;
    while (i < all_points.size()) {
      int64_t ts = all_points[i].ts_ns;
      row_fields.clear();

      // Collect all fields at this timestamp
      // String values need to stay alive until appendRecord completes,
      // so we reference directly into all_points (stable since we don't modify it).
      while (i < all_points.size() && all_points[i].ts_ns == ts) {
        const auto& pt = all_points[i];
        if (pt.is_string) {
          row_fields.push_back({
              .name = result.columns[pt.col_idx].name,
              .value = std::string_view(pt.string_val),
          });
        } else {
          row_fields.push_back({
              .name = result.columns[pt.col_idx].name,
              .value = pt.numeric_val,
          });
        }
        i++;
      }

      auto status = writeHost().appendRecord(
          *topic, PJ::Timestamp{ts},
          PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
      if (!status) return status;
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "Imported " + std::to_string(result.lines_processed) + " rows, " +
            std::to_string(result.lines_skipped) + " skipped");

    return PJ::okStatus();
  }

 private:
  CsvDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(CsvSource, kCsvManifest)

PJ_DIALOG_PLUGIN(CsvDialog)
