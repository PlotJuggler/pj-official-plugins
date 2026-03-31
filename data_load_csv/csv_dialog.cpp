#include "csv_dialog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// Generated from ui files at configure time
#include "dataload_csv_ui.hpp"
#include "datetimehelp_ui.hpp"

int CsvDialog::timeColumnIndex() const {
  if (time_mode_ == "column" && selected_column_index_ >= 0) {
    return selected_column_index_;
  }
  return -1;
}

int CsvDialog::combinedColumnIndex() const {
  if (time_mode_ == "combined") return combined_index_;
  return -1;
}

void CsvDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  analyzeFile();
}

std::string CsvDialog::manifest() const {
  return R"({"name":"CSV File Source","version":"1.0.0"})";
}

std::string CsvDialog::ui_content() const {
  return kDataLoadCsvUi;
}

std::string CsvDialog::widget_data() {
  PJ::WidgetData wd;

  // Delimiter
  std::vector<std::string> delimiters = {
      "\",\" (comma)", "\";\" (semicolon)", "\" \" (space)", "\"\\t\" (tab)"};
  wd.setItems("comboBox", delimiters);
  wd.setCurrentIndex("comboBox", delimiterToIndex(delimiter_));

  // Time axis radios
  wd.setChecked("radioButtonIndex", time_mode_ == "row_number");
  wd.setChecked("radioButtonSelect", time_mode_ == "column");
  wd.setChecked("radioButtonDateTimeColumns", time_mode_ == "combined");

  // Column list
  wd.setListItems("listWidgetSeries", column_names_);
  wd.setEnabled("listWidgetSeries", time_mode_ == "column");
  if (time_mode_ == "column" && selected_column_index_ >= 0 &&
      selected_column_index_ < static_cast<int>(column_names_.size())) {
    wd.setSelectedItems("listWidgetSeries",
                        {column_names_[static_cast<size_t>(selected_column_index_)]});
  }

  // Combined pairs
  std::vector<std::string> combo_items;
  for (const auto& p : combined_pairs_) combo_items.push_back(p.virtual_name);
  wd.setItems("combinedCombo", combo_items);
  if (combined_index_ >= 0) wd.setCurrentIndex("combinedCombo", combined_index_);
  wd.setEnabled("radioButtonDateTimeColumns", !combined_pairs_.empty());
  wd.setEnabled("combinedCombo", time_mode_ == "combined" && !combined_pairs_.empty());
  wd.setVisible("combinedCombo", !combined_pairs_.empty());

  // Timestamp format
  wd.setChecked("radioAutoTime", !use_custom_format_);
  wd.setChecked("radioCustomTime", use_custom_format_);
  wd.setText("lineEditDateFormat", custom_format_);
  wd.setEnabled("lineEditDateFormat", use_custom_format_);

  // Format live preview
  if (use_custom_format_ && !custom_format_.empty()) {
    wd.setText("labelFormatPreview",
               "Example (2024-01-15 14:30:45): " + computeFormatPreview(custom_format_));
  } else {
    wd.setText("labelFormatPreview", "");
  }

  // Preview table
  wd.setTableHeaders("tableView", column_names_);
  wd.setTableRows("tableView", preview_rows_);

  // OK enabled?
  bool ok = (time_mode_ == "row_number") ||
            (time_mode_ == "column" && selected_column_index_ >= 0) ||
            (time_mode_ == "combined" && combined_index_ >= 0);
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
  if (widget_name == "combinedCombo") {
    combined_index_ = index;
    return true;
  }
  return false;
}

bool CsvDialog::onToggled(std::string_view widget_name, bool checked) {
  if (!checked) return false;
  if (widget_name == "radioButtonIndex") { time_mode_ = "row_number"; return true; }
  if (widget_name == "radioButtonSelect") { time_mode_ = "column"; return true; }
  if (widget_name == "radioButtonDateTimeColumns") { time_mode_ = "combined"; return true; }
  if (widget_name == "radioAutoTime") { use_custom_format_ = false; return true; }
  if (widget_name == "radioCustomTime") { use_custom_format_ = true; return true; }
  return false;
}

bool CsvDialog::onSelectionChanged(std::string_view widget_name,
                                   const std::vector<std::string>& selected) {
  if (widget_name == "listWidgetSeries" && !selected.empty()) {
    for (int i = 0; i < static_cast<int>(column_names_.size()); i++) {
      if (column_names_[static_cast<size_t>(i)] == selected[0]) {
        selected_column_index_ = i;
        return true;
      }
    }
  }
  return false;
}

bool CsvDialog::onItemDoubleClicked(std::string_view widget_name, int index) {
  if (widget_name == "listWidgetSeries" && time_mode_ == "column" &&
      index >= 0 && index < static_cast<int>(column_names_.size())) {
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
  if (time_mode_ == "column" && selected_column_index_ >= 0 &&
      selected_column_index_ < static_cast<int>(column_names_.size())) {
    auto& name = column_names_[static_cast<size_t>(selected_column_index_)];
    column_history_.erase(
        std::remove(column_history_.begin(), column_history_.end(), name),
        column_history_.end());
    column_history_.insert(column_history_.begin(), name);
    if (column_history_.size() > 10) column_history_.resize(10);
  }
}

std::string CsvDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["filepath"] = filepath_;
  cfg["delimiter"] = std::string(1, delimiter_);
  cfg["time_mode"] = time_mode_;
  cfg["time_column_index"] = selected_column_index_;
  cfg["combined_column_index"] = combined_index_;
  cfg["custom_time_format"] = custom_format_;
  cfg["use_custom_format"] = use_custom_format_;
  cfg["column_history"] = column_history_;
  return cfg.dump();
}

bool CsvDialog::loadConfig(std::string_view config_json) {
  auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) return false;
  filepath_ = cfg.value("filepath", std::string{});
  auto d = cfg.value("delimiter", std::string(","));
  delimiter_ = d.empty() ? ',' : d[0];
  time_mode_ = cfg.value("time_mode", std::string("row_number"));
  selected_column_index_ = cfg.value("time_column_index", -1);
  combined_index_ = cfg.value("combined_column_index", -1);
  custom_format_ = cfg.value("custom_time_format", std::string{});
  use_custom_format_ = cfg.value("use_custom_format", false);
  if (cfg.contains("column_history") && cfg["column_history"].is_array()) {
    column_history_ = cfg["column_history"].get<std::vector<std::string>>();
  }
  // Validate: a mode that requires a selection is useless without one — fall back to row_number
  if (time_mode_ == "column" && selected_column_index_ < 0) time_mode_ = "row_number";
  if (time_mode_ == "combined" && combined_index_ < 0) time_mode_ = "row_number";
  if (!filepath_.empty()) analyzeFile();
  return true;
}

void CsvDialog::analyzeFile() {
  column_names_.clear();
  combined_pairs_.clear();
  preview_rows_.clear();

  if (filepath_.empty()) return;
  std::ifstream file(filepath_);
  if (!file.is_open()) return;

  // Read header
  std::string header_line;
  if (!std::getline(file, header_line)) return;
  if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();

  if (delimiter_ == '\0') delimiter_ = PJ::CSV::DetectDelimiter(header_line);

  column_names_ = PJ::CSV::ParseHeaderLine(header_line, delimiter_);

  // Build preview rows: first 100 data lines (like the original)
  std::string line;
  std::vector<std::string> parts;
  std::vector<PJ::CSV::ColumnTypeInfo> first_row_types;
  int count = 0;
  while (std::getline(file, line) && count < 100) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    PJ::CSV::SplitLine(line, delimiter_, parts);
    preview_rows_.push_back(parts);

    // Detect types from first row
    if (count == 0) {
      first_row_types.resize(column_names_.size());
      for (size_t i = 0; i < parts.size() && i < first_row_types.size(); i++) {
        if (!parts[i].empty()) first_row_types[i] = PJ::CSV::DetectColumnType(parts[i]);
      }
      combined_pairs_ =
          PJ::CSV::DetectCombinedDateTimeColumns(column_names_, first_row_types);
    }
    count++;
  }

  // If the current mode was "combined" but this file has no detectable pairs,
  // fall back to row_number to avoid an invalid state.
  if (time_mode_ == "combined" && combined_pairs_.empty()) {
    time_mode_ = "row_number";
    combined_index_ = -1;
  }
  // Auto-select the first combined pair if none is selected yet.
  if (!combined_pairs_.empty() && combined_index_ < 0) {
    combined_index_ = 0;
  }
}

// Interprets a Qt date/time format string against the fixed example
// 2024-01-15 14:30:45 and returns the formatted result.
// Supports: yyyy yy MMMM MMM MM M dddd ddd dd d HH H hh h mm m ss s zzz z AP ap A a
// and single-quoted literals.
std::string CsvDialog::computeFormatPreview(const std::string& fmt) {
  const int year = 2024, month = 1, day = 15, hour = 14, min = 30, sec = 45;
  const int weekday = 0;  // Monday (2024-01-15)

  static const char* month_long[] = {"January",  "February", "March",    "April",
                                     "May",       "June",     "July",     "August",
                                     "September", "October",  "November", "December"};
  static const char* month_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char* wday_long[] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                    "Friday",   "Saturday", "Sunday"};
  static const char* wday_short[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

  auto pad2 = [](int v) -> std::string { return (v < 10 ? "0" : "") + std::to_string(v); };
  auto match = [&](size_t pos, const char* tok) -> bool {
    size_t n = 0;
    while (tok[n]) ++n;
    return fmt.size() - pos >= n && fmt.compare(pos, n, tok) == 0;
  };

  std::string out;
  size_t i = 0;
  while (i < fmt.size()) {
    if (fmt[i] == '\'') {
      ++i;
      while (i < fmt.size() && fmt[i] != '\'') out += fmt[i++];
      if (i < fmt.size()) ++i;
    } else if (match(i, "yyyy"))  { out += std::to_string(year); i += 4; }
    else if (match(i, "yy"))      { out += pad2(year % 100); i += 2; }
    else if (match(i, "MMMM"))    { out += month_long[month - 1]; i += 4; }
    else if (match(i, "MMM"))     { out += month_short[month - 1]; i += 3; }
    else if (match(i, "MM"))      { out += pad2(month); i += 2; }
    else if (fmt[i] == 'M')       { out += std::to_string(month); ++i; }
    else if (match(i, "dddd"))    { out += wday_long[weekday]; i += 4; }
    else if (match(i, "ddd"))     { out += wday_short[weekday]; i += 3; }
    else if (match(i, "dd"))      { out += pad2(day); i += 2; }
    else if (fmt[i] == 'd')       { out += std::to_string(day); ++i; }
    else if (match(i, "HH"))      { out += pad2(hour); i += 2; }
    else if (fmt[i] == 'H')       { out += std::to_string(hour); ++i; }
    else if (match(i, "hh"))      { int h = hour % 12; if (!h) h = 12; out += pad2(h); i += 2; }
    else if (fmt[i] == 'h')       { int h = hour % 12; if (!h) h = 12; out += std::to_string(h); ++i; }
    else if (match(i, "mm"))      { out += pad2(min); i += 2; }
    else if (fmt[i] == 'm')       { out += std::to_string(min); ++i; }
    else if (match(i, "ss"))      { out += pad2(sec); i += 2; }
    else if (fmt[i] == 's')       { out += std::to_string(sec); ++i; }
    else if (match(i, "zzz"))     { out += "000"; i += 3; }
    else if (fmt[i] == 'z')       { out += "0"; ++i; }
    else if (match(i, "AP"))      { out += (hour < 12 ? "AM" : "PM"); i += 2; }
    else if (match(i, "ap"))      { out += (hour < 12 ? "am" : "pm"); i += 2; }
    else if (fmt[i] == 'A')       { out += (hour < 12 ? "AM" : "PM"); ++i; }
    else if (fmt[i] == 'a')       { out += (hour < 12 ? "am" : "pm"); ++i; }
    else                          { out += fmt[i++]; }
  }
  return out;
}

int CsvDialog::delimiterToIndex(char d) {
  switch (d) {
    case ',': return 0;
    case ';': return 1;
    case ' ': return 2;
    case '\t': return 3;
    default: return 0;
  }
}

char CsvDialog::indexToDelimiter(int idx) {
  switch (idx) {
    case 0: return ',';
    case 1: return ';';
    case 2: return ' ';
    case 3: return '\t';
    default: return ',';
  }
}
