#include "csv_dialog.hpp"

// Generated at configure time
#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "csv_manifest.hpp"
#include "dataload_csv_ui.hpp"
#include "datetimehelp_ui.hpp"

int CsvDialog::timeColumnIndex() const {
  if (time_mode_ == TimeMode::Column && selected_column_index_ >= 0) {
    return selected_column_index_;
  }
  return -1;
}

int CsvDialog::combinedColumnIndex() const {
  if (time_mode_ == TimeMode::Combined) {
    return combined_index_;
  }
  return -1;
}

bool CsvDialog::combinableAvailable() const {
  bool has_date = false;
  bool has_time = false;
  for (const auto& info : column_types_) {
    has_date = has_date || info.type == PJ::CSV::ColumnType::DATE_ONLY;
    has_time = has_time || info.type == PJ::CSV::ColumnType::TIME_ONLY;
  }
  return has_date && has_time;
}

void CsvDialog::setCombinedColumns(int date_index, int time_index) {
  combined_pairs_.clear();
  combined_index_ = -1;
  const int n = static_cast<int>(column_names_.size());
  if (date_index >= 0 && time_index >= 0 && date_index < n && time_index < n) {
    PJ::CSV::CombinedColumnPair pair;
    pair.date_column_index = date_index;
    pair.time_column_index = time_index;
    pair.virtual_name =
        column_names_[static_cast<size_t>(date_index)] + " + " + column_names_[static_cast<size_t>(time_index)];
    combined_pairs_.push_back(pair);
    combined_index_ = 0;
  }
}

void CsvDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  analyzeFile();
}

std::string CsvDialog::manifest() const {
  return kCsvManifest;
}

std::string CsvDialog::ui_content() const {
  return kDataLoadCsvUi;
}

std::string CsvDialog::widget_data() {
  PJ::WidgetData wd;

  // Delimiter: the "Detect Automatically" toggle (host-adapted into an iOS
  // switch) gates the dropdown. Auto on -> dropdown shows the detected delimiter
  // and is disabled; auto off (incl. the forced-off ambiguous case) -> dropdown
  // is enabled for the user to pick. An ambiguous header also disables the
  // toggle itself, so auto cannot be re-armed until a different file is loaded.
  std::vector<std::string> delimiters = {"\",\" (comma)", "\";\" (semicolon)", "\" \" (space)", "\"\\t\" (tab)"};
  wd.setItems("comboBox", delimiters);
  wd.setCurrentIndex("comboBox", delimiterToIndex(delimiter_));
  wd.setChecked("checkBoxAutoDelim", detect_delimiter_);
  wd.setEnabled("checkBoxAutoDelim", !delimiter_ambiguous_);
  wd.setEnabled("comboBox", !detect_delimiter_);

  // Time-axis mode dropdown (docked on the right of the "Time Axis" header).
  // "Multiple Columns" only appears when the file has both a date-only and a
  // time-only column, so a combined timestamp can be formed from them.
  std::vector<std::string> mode_items = {"Number of Rows", "Selected Column"};
  if (combinableAvailable()) {
    mode_items.push_back("Multiple Columns");
  }
  wd.setItems("timeAxisModeCombo", mode_items);
  int mode_index = 0;
  if (time_mode_ == TimeMode::Column) {
    mode_index = 1;
  } else if (time_mode_ == TimeMode::Combined) {
    mode_index = 2;
  }
  wd.setCurrentIndex("timeAxisModeCombo", mode_index);

  // Column list. "Selected Column" picks one time column (single-select).
  // "Multiple Columns" multi-selects the date + time columns that form the
  // combined timestamp; every non date/time column is greyed out.
  const bool combined_mode = time_mode_ == TimeMode::Combined;
  wd.setListItems("listWidgetSeries", column_names_);
  wd.setEnabled("listWidgetSeries", time_mode_ == TimeMode::Column || combined_mode);
  wd.setListSelectionMode("listWidgetSeries", combined_mode);
  if (combined_mode) {
    std::vector<std::string> disabled;
    for (size_t i = 0; i < column_names_.size(); i++) {
      const bool time_capable = i < column_types_.size() && (column_types_[i].type == PJ::CSV::ColumnType::DATE_ONLY ||
                                                             column_types_[i].type == PJ::CSV::ColumnType::TIME_ONLY);
      if (!time_capable) {
        disabled.push_back(column_names_[i]);
      }
    }
    wd.setListItemsDisabled("listWidgetSeries", disabled);
    std::vector<std::string> selected;
    if (!combined_pairs_.empty()) {
      const auto& p = combined_pairs_.front();
      selected.push_back(column_names_[static_cast<size_t>(p.date_column_index)]);
      selected.push_back(column_names_[static_cast<size_t>(p.time_column_index)]);
    }
    wd.setSelectedItems("listWidgetSeries", selected);
  } else {
    wd.setListItemsDisabled("listWidgetSeries", {});
    std::vector<std::string> selected;
    if (time_mode_ == TimeMode::Column && selected_column_index_ >= 0 &&
        selected_column_index_ < static_cast<int>(column_names_.size())) {
      selected.push_back(column_names_[static_cast<size_t>(selected_column_index_)]);
    }
    wd.setSelectedItems("listWidgetSeries", selected);
  }

  // Timestamp format: the "Detect Automatically" toggle (host-adapted from the
  // checkbox into an iOS switch) gates the custom-format row — turning it off
  // enables the "Custom" label + format field below.
  wd.setChecked("checkBoxAutoTime", !use_custom_format_);
  wd.setEnabled("labelCustomFormat", use_custom_format_);
  wd.setText("lineEditDateFormat", custom_format_);
  wd.setEnabled("lineEditDateFormat", use_custom_format_);

  // Preview table
  wd.setTableHeaders("tableView", column_names_);
  wd.setTableRows("tableView", preview_rows_);

  // Raw-text preview (header + first 100 lines, verbatim)
  wd.setPlainText("rawText", raw_preview_);

  // Preview mode: the segmented Table/Raw control (host-adapted from the two
  // grouped radios in the Preview band) drives which preview widget is shown.
  wd.setChecked("radioPreviewTable", !preview_raw_);
  wd.setChecked("radioPreviewRaw", preview_raw_);
  wd.setVisible("tableView", !preview_raw_);
  wd.setVisible("rawText", preview_raw_);

  // Preview warning (shared label — text set by analyzeFile)
  wd.setVisible("labelWarning", !warning_message_.empty());
  if (!warning_message_.empty()) {
    wd.setText("labelWarning", warning_message_);
  }

  // OK enabled?
  bool ok = (time_mode_ == TimeMode::RowNumber) || (time_mode_ == TimeMode::Column && selected_column_index_ >= 0) ||
            (time_mode_ == TimeMode::Combined && combined_index_ >= 0);
  wd.setOkEnabled("buttonBox", ok);

  if (accept_requested_) {
    accept_requested_ = false;
    wd.requestAccept();
  }

  if (show_help_requested_) {
    show_help_requested_ = false;
    wd.requestSubDialog(kDateTimeHelpUi);
  }

  return wd.toJson();
}

bool CsvDialog::onIndexChanged(std::string_view widget_name, int index) {
  if (widget_name == "comboBox") {
    delimiter_ = indexToDelimiter(index);
    analyzeFile();
    return true;
  }
  if (widget_name == "timeAxisModeCombo") {
    // Item order matches widget_data: 0 = row number, 1 = single column,
    // 2 = multiple columns (present only when a date + time column exist).
    time_mode_ = index == 1 ? TimeMode::Column : index == 2 ? TimeMode::Combined : TimeMode::RowNumber;
    // Re-run so the preview warning is recomputed for the new mode (the
    // non-monotonic warning only applies in single-column mode) instead of
    // lingering from the previous mode's selection.
    analyzeFile();
    return true;
  }
  return false;
}

bool CsvDialog::onToggled(std::string_view widget_name, bool checked) {
  // The auto-detect toggles act in both directions (unlike a radio), so they are
  // handled before the checked-only radios below.
  if (widget_name == "checkBoxAutoDelim") {
    detect_delimiter_ = checked;
    analyzeFile();
    return true;
  }
  if (widget_name == "checkBoxAutoTime") {
    use_custom_format_ = !checked;
    return true;
  }
  if (!checked) {
    return false;
  }
  if (widget_name == "radioPreviewTable") {
    preview_raw_ = false;
    return true;
  }
  if (widget_name == "radioPreviewRaw") {
    preview_raw_ = true;
    return true;
  }
  return false;
}

bool CsvDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "listWidgetSeries") {
    return false;
  }
  if (time_mode_ == TimeMode::Combined) {
    // Multiple Columns: from the multi-selection, take the first date-only and
    // first time-only column and combine them into the timestamp.
    int date_idx = -1;
    int time_idx = -1;
    for (const auto& name : selected) {
      for (size_t i = 0; i < column_names_.size() && i < column_types_.size(); i++) {
        if (column_names_[i] != name) {
          continue;
        }
        if (date_idx < 0 && column_types_[i].type == PJ::CSV::ColumnType::DATE_ONLY) {
          date_idx = static_cast<int>(i);
        } else if (time_idx < 0 && column_types_[i].type == PJ::CSV::ColumnType::TIME_ONLY) {
          time_idx = static_cast<int>(i);
        }
      }
    }
    setCombinedColumns(date_idx, time_idx);
    return true;
  }
  if (!selected.empty()) {
    for (int i = 0; i < static_cast<int>(column_names_.size()); i++) {
      if (column_names_[static_cast<size_t>(i)] == selected[0]) {
        selected_column_index_ = i;
        analyzeFile();
        return true;
      }
    }
  }
  return false;
}

bool CsvDialog::onItemDoubleClicked(std::string_view widget_name, int index) {
  if (widget_name == "listWidgetSeries" && time_mode_ == TimeMode::Column && index >= 0 &&
      index < static_cast<int>(column_names_.size())) {
    selected_column_index_ = index;
    accept_requested_ = true;
    return true;
  }
  return false;
}

bool CsvDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "dateTimeHelpButton") {
    show_help_requested_ = true;
    return true;
  }
  return false;
}

bool CsvDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  if (widget_name == "lineEditDateFormat") {
    custom_format_ = std::string(text);
    return true;
  }
  return false;
}

void CsvDialog::onAccepted(std::string_view /*json*/) {
  if (time_mode_ == TimeMode::Column && selected_column_index_ >= 0 &&
      selected_column_index_ < static_cast<int>(column_names_.size())) {
    auto& name = column_names_[static_cast<size_t>(selected_column_index_)];
    column_history_.erase(std::remove(column_history_.begin(), column_history_.end(), name), column_history_.end());
    column_history_.insert(column_history_.begin(), name);
    if (column_history_.size() > 10) {
      column_history_.resize(10);
    }
  }
}

std::string CsvDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["filepath"] = filepath_;
  cfg["delimiter"] = std::string(1, delimiter_);
  cfg["time_mode"] = timeModeToString(time_mode_);
  cfg["time_column_index"] = selected_column_index_;
  cfg["combined_date_index"] = combined_pairs_.empty() ? -1 : combined_pairs_.front().date_column_index;
  cfg["combined_time_index"] = combined_pairs_.empty() ? -1 : combined_pairs_.front().time_column_index;
  cfg["custom_time_format"] = custom_format_;
  cfg["use_custom_format"] = use_custom_format_;
  cfg["detect_delimiter"] = detect_delimiter_;
  cfg["column_history"] = column_history_;
  return cfg.dump();
}

bool CsvDialog::loadConfig(std::string_view config_json) {
  auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) {
    return false;
  }
  filepath_ = cfg.value("filepath", std::string{});
  auto d = cfg.value("delimiter", std::string(","));
  delimiter_ = d.empty() ? ',' : d[0];
  time_mode_ = stringToTimeMode(cfg.value("time_mode", std::string("row_number")));
  selected_column_index_ = cfg.value("time_column_index", -1);
  const int restored_combined_date = cfg.value("combined_date_index", -1);
  const int restored_combined_time = cfg.value("combined_time_index", -1);
  custom_format_ = cfg.value("custom_time_format", std::string{});
  use_custom_format_ = cfg.value("use_custom_format", false);
  detect_delimiter_ = cfg.value("detect_delimiter", true);
  if (cfg.contains("column_history") && cfg["column_history"].is_array()) {
    column_history_ = cfg["column_history"].get<std::vector<std::string>>();
  }
  // Validate: a mode that requires a selection is useless without one — fall back to row_number
  if (time_mode_ == TimeMode::Column && selected_column_index_ < 0) {
    time_mode_ = TimeMode::RowNumber;
  }
  if (time_mode_ == TimeMode::Combined && (restored_combined_date < 0 || restored_combined_time < 0)) {
    time_mode_ = TimeMode::RowNumber;
  }
  if (!filepath_.empty()) {
    analyzeFile();
  }
  // Re-apply the saved combined pair after the columns/types are rebuilt.
  if (time_mode_ == TimeMode::Combined) {
    setCombinedColumns(restored_combined_date, restored_combined_time);
  }
  return true;
}

void CsvDialog::analyzeFile() {
  // Remember the selected time column by NAME so a delimiter change (which
  // re-splits the header) can re-resolve it against the rebuilt columns below.
  // Only meaningful on a RE-analyze: on the first analyze there are no columns
  // yet, and selected_column_index_ may hold an index restored from saveConfig.
  const bool had_columns = !column_names_.empty();
  std::string previously_selected_column;
  if (had_columns && selected_column_index_ >= 0 && selected_column_index_ < static_cast<int>(column_names_.size())) {
    previously_selected_column = column_names_[static_cast<size_t>(selected_column_index_)];
  }

  column_names_.clear();
  column_types_.clear();
  preview_rows_.clear();
  raw_preview_.clear();

  if (filepath_.empty()) {
    return;
  }
  std::ifstream file(filepath_);
  if (!file.is_open()) {
    return;
  }

  // Read header
  std::string header_line;
  if (!std::getline(file, header_line)) {
    return;
  }
  if (!header_line.empty() && header_line.back() == '\r') {
    header_line.pop_back();
  }

  // Delimiter detection drives the "Detect Automatically" toggle. Ambiguity is a
  // property of the header, so it is always recomputed. With auto on: a single
  // clear delimiter is adopted; an ambiguous header drops to manual so the user
  // disambiguates. With auto off, the user's chosen delimiter is respected.
  const auto detected = PJ::CSV::DetectDelimiterEx(header_line);
  delimiter_ambiguous_ = detected.ambiguous;
  if (detect_delimiter_) {
    if (detected.ambiguous) {
      detect_delimiter_ = false;
      if (delimiter_ == '\0') {
        delimiter_ = detected.delimiter;
      }
    } else {
      delimiter_ = detected.delimiter;
    }
  } else if (delimiter_ == '\0') {
    delimiter_ = detected.delimiter;
  }

  // Check for duplicate column names before deduplication
  {
    std::vector<std::string> raw_parts;
    PJ::CSV::SplitLine(header_line, delimiter_, raw_parts);
    std::set<std::string> seen;
    has_duplicate_columns_ = false;
    for (const auto& part : raw_parts) {
      if (!seen.insert(part).second) {
        has_duplicate_columns_ = true;
        break;
      }
    }
  }

  column_names_ = PJ::CSV::ParseHeaderLine(header_line, delimiter_);

  // Raw-text preview mirrors the file verbatim: the header plus the first 100
  // data lines, exactly as read (the Raw Text tab shows this).
  raw_preview_ = header_line;

  // Build preview rows: first 100 data lines (like the original)
  std::string line;
  std::vector<std::string> parts;
  std::vector<PJ::CSV::ColumnTypeInfo> first_row_types;
  int count = 0;
  while (std::getline(file, line) && count < 100) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    raw_preview_ += '\n';
    raw_preview_ += line;
    PJ::CSV::SplitLine(line, delimiter_, parts);
    preview_rows_.push_back(parts);

    // Detect types from first row
    if (count == 0) {
      first_row_types.resize(column_names_.size());
      for (size_t i = 0; i < parts.size() && i < first_row_types.size(); i++) {
        if (!parts[i].empty()) {
          first_row_types[i] = PJ::CSV::DetectColumnType(parts[i]);
        }
      }
      column_types_ = first_row_types;
    }
    count++;
  }

  // Compute preview warning message
  warning_message_.clear();
  // Check for duplicate column names in header
  if (has_duplicate_columns_) {
    warning_message_ = "Duplicate column names detected. Adding suffixes";
  }
  // Check for rows with wrong column count
  if (warning_message_.empty()) {
    for (const auto& row : preview_rows_) {
      if (row.size() != column_names_.size()) {
        warning_message_ = "Some rows have column number mismatch. Will skip those";
        break;
      }
    }
  }
  // Check for non-monotonic timestamps in selected column (numeric only)
  if (warning_message_.empty() && time_mode_ == TimeMode::Column && selected_column_index_ >= 0 &&
      selected_column_index_ < static_cast<int>(column_names_.size())) {
    double prev_val = std::numeric_limits<double>::lowest();
    for (const auto& row : preview_rows_) {
      auto col = static_cast<size_t>(selected_column_index_);
      if (col >= row.size() || row[col].empty()) {
        continue;
      }
      auto val = PJ::CSV::toDouble(PJ::CSV::Trim(row[col]));
      if (!val) {
        break;  // Non-numeric timestamps: skip check
      }
      if (*val < prev_val) {
        warning_message_ =
            "Values in \"" + column_names_[col] + "\" are not monotonically increasing. Will sort these automatically";
        break;
      }
      prev_val = *val;
    }
  }

  // Multiple-Columns mode needs a date column and a time column; drop to row
  // number if this file has neither. The user's picked pair carries over across
  // a re-analyze, but is dropped if its column indices no longer fit.
  if (time_mode_ == TimeMode::Combined && !combinableAvailable()) {
    time_mode_ = TimeMode::RowNumber;
    combined_pairs_.clear();
    combined_index_ = -1;
  }
  if (!combined_pairs_.empty()) {
    const auto& p = combined_pairs_.front();
    const int n = static_cast<int>(column_names_.size());
    if (p.date_column_index >= n || p.time_column_index >= n) {
      combined_pairs_.clear();
      combined_index_ = -1;
    }
  }

  // Re-validate the selected time column against the rebuilt columns so we never
  // keep an index that points past the new column count (which would silently
  // load the row number as the time axis). On a RE-analyze (delimiter change)
  // re-resolve the previous column by name; otherwise just clamp the restored
  // index. Dropping the selection to -1 disables OK until the user re-picks.
  if (time_mode_ == TimeMode::Column) {
    if (had_columns) {
      selected_column_index_ = -1;
      for (size_t i = 0; i < column_names_.size(); i++) {
        if (column_names_[i] == previously_selected_column) {
          selected_column_index_ = static_cast<int>(i);
          break;
        }
      }
    } else if (selected_column_index_ >= static_cast<int>(column_names_.size())) {
      selected_column_index_ = -1;
    }
  }
}

int CsvDialog::delimiterToIndex(char d) {
  switch (d) {
    case ',':
      return 0;
    case ';':
      return 1;
    case ' ':
      return 2;
    case '\t':
      return 3;
    default:
      return 0;
  }
}

char CsvDialog::indexToDelimiter(int idx) {
  switch (idx) {
    case 0:
      return ',';
    case 1:
      return ';';
    case 2:
      return ' ';
    case 3:
      return '\t';
    default:
      return ',';
  }
}
