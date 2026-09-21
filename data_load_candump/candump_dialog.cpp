#include "candump_dialog.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/time_format.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "candump_parser.hpp"

// Generated at configure time.
#include "candump_dialog_ui.hpp"
#include "candump_manifest.hpp"

namespace candump_detail {

namespace {

/// Renders `value` with "," thousands separators (e.g. 16000 -> "16,000").
/// No locale dependence, matching this file's existing no-double/no-locale
/// number formatting.
std::string formatThousands(std::uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  const std::size_t n = digits.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (i > 0 && (n - i) % 3 == 0) {
      out += ',';
    }
    out += digits[i];
  }
  return out;
}

/// Renders a non-negative nanosecond duration as "<seconds>.<decisecond> s"
/// (e.g. "40.0 s"), using integer arithmetic only.
std::string formatSecondsOneDecimal(std::int64_t duration_ns) {
  const std::uint64_t mag = duration_ns > 0 ? static_cast<std::uint64_t>(duration_ns) : 0;
  const std::uint64_t whole = mag / 1'000'000'000ull;
  const std::uint64_t tenths = (mag % 1'000'000'000ull) / 100'000'000ull;
  return formatThousands(whole) + "." + std::to_string(tenths) + " s";
}

/// UTC calendar date-time "YYYY-MM-DD HH:MM:SS" (PJ::formatIso8601Utc with
/// its "T" separator swapped for a space) -- the SDK's own no-locale
/// date-time formatter, so this stays consistent with how the rest of the
/// host displays absolute timestamps.
std::string formatDateTime(std::int64_t ts_ns) {
  std::string iso = PJ::formatIso8601Utc(ts_ns);
  if (iso.size() > 10) {
    iso[10] = ' ';
  }
  return iso;
}

/// Basename of a filesystem path (text after the last '/' or '\', or the
/// whole string if neither is present).
std::string basenameOf(const std::string& path) {
  const std::size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

/// Runs one of RecognizedCounters' `append*(std::string&) const` helpers --
/// shared with CandumpSource's own one-line summary, where each writes
/// "; <text>" as a continuation -- and, if it produced anything, appends its
/// text as its OWN line instead (stripping the "; " continuation marker).
/// Keeps the wording identical to CandumpSource's summary while giving the
/// dialog's multi-line preview one warning per line.
void appendCounterLine(
    std::vector<std::string>& lines, void (RecognizedCounters::*appender)(std::string&) const,
    const RecognizedCounters& counters) {
  std::string fragment;
  (counters.*appender)(fragment);
  if (!fragment.empty()) {
    lines.push_back(fragment.size() > 2 ? fragment.substr(2) : fragment);
  }
}

/// Reads the last `tail_bytes` of `path` and returns the raw timestamp of
/// the LAST line (scanning backwards) that carries a numeric timestamp.
/// Deliberately independent of the forward prescan below: a 200k-line
/// prescan cap can stop long before the end of a multi-million-line file,
/// so the displayed "end of capture" time comes from here instead.
std::optional<std::int64_t> readTailLastTimestampNs(const std::string& path, std::uint64_t tail_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return std::nullopt;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0) {
    return std::nullopt;
  }
  const std::streamoff start =
      size > static_cast<std::streamoff>(tail_bytes) ? size - static_cast<std::streamoff>(tail_bytes) : 0;
  file.seekg(start);
  const std::string tail((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  std::vector<std::string_view> lines;
  std::size_t pos = 0;
  while (pos < tail.size()) {
    const std::size_t nl = tail.find('\n', pos);
    const std::size_t end = nl == std::string::npos ? tail.size() : nl;
    lines.push_back(std::string_view(tail).substr(pos, end - pos));
    if (nl == std::string::npos) {
      break;
    }
    pos = nl + 1;
  }
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (it->empty()) {
      continue;
    }
    const ParsedLine parsed = parseLine(*it);
    if (parsed.has_timestamp && parsed.kind != LineKind::kWallClockTs && parsed.kind != LineKind::kMalformed) {
      return rawTimestampNs(parsed);
    }
  }
  return std::nullopt;
}

}  // namespace

void CandumpDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  scanFile();
  needs_scan_ = false;
}

void CandumpDialog::setInterfaceDicts(std::map<std::string, std::vector<std::string>> iface_dicts) {
  iface_dicts_ = std::move(iface_dicts);
}

std::optional<TimeMode> CandumpDialog::timeModeOverride() const {
  switch (time_mode_override_) {
    case 1:
      return TimeMode::kAbsolute;
    case 2:
      return TimeMode::kRelativeMonotonic;
    case 3:
      return TimeMode::kRelativeDelta;
    default:
      return std::nullopt;
  }
}

std::string CandumpDialog::manifest() const {
  return kCandumpManifest;
}

std::string CandumpDialog::ui_content() const {
  return kCandumpDialogUi;
}

std::string CandumpDialog::currentInterface() const {
  if (selected_interface_index_ >= 0 && selected_interface_index_ < static_cast<int>(interface_names_.size())) {
    return interface_names_[static_cast<std::size_t>(selected_interface_index_)];
  }
  return {};
}

std::string CandumpDialog::widget_data() {
  if (needs_scan_) {
    scanFile();
    needs_scan_ = false;
  }

  PJ::WidgetData wd;
  wd.setText("labelSummary", summary_);
  wd.setTableHeaders("tableInterfaces", {"Interface", "Frames", "Messages", "Dictionary"});
  // The Dictionary column follows the live assignment (the picker edits it
  // after the scan), so it is filled here rather than in scanFile().
  constexpr std::size_t kDictionaryColumn = 3;
  auto rows = interface_rows_;
  std::vector<std::string> dict_paths(rows.size());
  for (std::size_t row = 0; row < rows.size(); ++row) {
    if (const auto it = iface_dicts_.find(interface_names_[row]); it != iface_dicts_.end() && !it->second.empty()) {
      dict_paths[row] = it->second.front();
    }
    rows[row][kDictionaryColumn] = dict_paths[row].empty() ? "none" : basenameOf(dict_paths[row]);
  }
  wd.setTableRows("tableInterfaces", rows);
  for (std::size_t row = 0; row < dict_paths.size(); ++row) {
    if (!dict_paths[row].empty()) {
      wd.setCellTooltip("tableInterfaces", static_cast<int>(row), static_cast<int>(kDictionaryColumn), dict_paths[row]);
    }
  }

  wd.setItems("comboInterface", interface_names_);
  wd.setCurrentIndex("comboInterface", selected_interface_index_);

  wd.setFilePicker("buttonDictionary", "Choose .dbc / .csv...", "*.dbc *.csv", "Select a .dbc or CSV signal table");
  const std::string iface = currentInterface();
  std::string dict_label = "none";
  if (!iface.empty()) {
    std::string filename = "none";
    if (const auto it = iface_dicts_.find(iface); it != iface_dicts_.end() && !it->second.empty()) {
      filename = basenameOf(it->second.front());
      for (std::size_t k = 1; k < it->second.size(); ++k) {
        filename += "; " + basenameOf(it->second[k]);
      }
    }
    dict_label = "Dictionary for " + iface + ": " + filename;
  }
  wd.setText("labelDictionary", dict_label);
  wd.setEnabled("buttonDictionary", !iface.empty());
  wd.setEnabled("buttonClearDictionary", !iface.empty());

  wd.setChecked("checkRawUnassigned", raw_unassigned_);

  const std::string auto_item =
      detected_time_mode_text_.empty() ? "Automatic" : "Automatic (" + detected_time_mode_text_ + ")";
  wd.setItems("comboTimeMode", {auto_item, "Absolute", "Relative to start (-tz)", "Delta between frames (-td)"});
  wd.setCurrentIndex("comboTimeMode", time_mode_override_);

  return wd.toJson();
}

std::string CandumpDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["filepath"] = filepath_;
  nlohmann::json mapping = nlohmann::json::object();
  for (const auto& [iface, paths] : iface_dicts_) {
    if (!paths.empty()) {
      mapping[iface] = paths;
    }
  }
  cfg["iface_dicts"] = mapping;
  cfg["raw_unassigned"] = raw_unassigned_;
  cfg["time_mode"] = time_mode_override_;
  return cfg.dump();
}

bool CandumpDialog::isValidInterfaceKey(std::string_view key) {
  if (key.empty() || key.size() > 15) {
    return false;
  }
  return key.find('/') == std::string_view::npos;
}

bool CandumpDialog::loadConfig(std::string_view config_json) {
  return applyConfigJson(config_json, /*scan_now=*/true);
}

bool CandumpDialog::loadConfigDeferringScan(std::string_view config_json) {
  return applyConfigJson(config_json, /*scan_now=*/false);
}

bool CandumpDialog::applyConfigJson(std::string_view config_json, bool scan_now) {
  const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) {
    return false;
  }
  filepath_ = cfg.value("filepath", std::string{});
  raw_unassigned_ = cfg.value("raw_unassigned", true);
  time_mode_override_ = cfg.value("time_mode", 0);
  if (time_mode_override_ < 0 || time_mode_override_ > 3) {
    time_mode_override_ = 0;
  }
  iface_dicts_.clear();
  if (cfg.contains("iface_dicts") && cfg["iface_dicts"].is_object()) {
    for (const auto& [key, value] : cfg["iface_dicts"].items()) {
      if (!isValidInterfaceKey(key) || !value.is_array()) {
        continue;  // hand-edited config: skip the bad entry, keep the rest
      }
      auto& paths = iface_dicts_[key];
      for (const auto& entry : value) {
        if (entry.is_string()) {
          paths.push_back(entry.get<std::string>());
        }
      }
    }
  }
  if (!filepath_.empty()) {
    if (scan_now) {
      scanFile();
      needs_scan_ = false;
    } else {
      needs_scan_ = true;
    }
  }
  return true;
}

bool CandumpDialog::onFileSelected(std::string_view widget_name, std::string_view path) {
  if (widget_name != "buttonDictionary") {
    return false;
  }
  const std::string iface = currentInterface();
  if (iface.empty()) {
    return false;
  }
  // The single picker edits the interface as a whole: replace its list.
  iface_dicts_[iface] = {std::string(path)};
  return true;
}

bool CandumpDialog::onClicked(std::string_view widget_name) {
  if (widget_name != "buttonClearDictionary") {
    return false;
  }
  const std::string iface = currentInterface();
  if (iface.empty()) {
    return false;
  }
  iface_dicts_.erase(iface);
  return true;
}

bool CandumpDialog::onToggled(std::string_view widget_name, bool checked) {
  if (widget_name != "checkRawUnassigned") {
    return false;
  }
  raw_unassigned_ = checked;
  return true;
}

bool CandumpDialog::onIndexChanged(std::string_view widget_name, int index) {
  if (widget_name == "comboInterface") {
    selected_interface_index_ = index;
    return true;
  }
  if (widget_name == "comboTimeMode") {
    time_mode_override_ = index;
    return true;
  }
  return false;
}

void CandumpDialog::scanFile() {
  summary_.clear();
  interface_names_.clear();
  interface_rows_.clear();
  detected_time_mode_text_.clear();
  selected_interface_index_ = 0;
  if (filepath_.empty()) {
    return;
  }

  std::ifstream file(filepath_);
  if (!file.is_open()) {
    summary_ = "Failed to open file.";
    return;
  }

  // The one bounded-scan pass: interface/grammar/malformed counts and the
  // TimeMode decision, shared verbatim with CandumpSource::importData()'s
  // own (smaller) use of the same function.
  const PrescanResult scan = prescanCandump(file, kScanCap);

  std::uint64_t total_frames = 0;
  interface_names_.reserve(scan.interfaces.size());
  interface_rows_.reserve(scan.interfaces.size());
  for (const auto& iface : scan.interfaces) {
    total_frames += iface.frames;
    interface_names_.push_back(iface.interface);
    // Dictionary column (index 3) is filled per render in widget_data().
    interface_rows_.push_back(
        {iface.interface, formatThousands(iface.frames) + (scan.truncated ? "+" : ""),
         formatThousands(iface.distinct_ids), std::string{}});
  }

  if (scan.lines_scanned == 0) {
    summary_ = "Empty file.";
    return;
  }
  if (scan.malformed == scan.lines_scanned) {
    summary_ = "This does not look like a candump capture (record with `candump -l` or `-t z/d/a`).";
    return;
  }

  if (scan.time_mode.saw_numeric_timestamp) {
    switch (scan.time_mode.mode) {
      case TimeMode::kAbsolute:
        detected_time_mode_text_ = "absolute";
        break;
      case TimeMode::kRelativeMonotonic:
        detected_time_mode_text_ = "relative";
        break;
      case TimeMode::kRelativeDelta:
        detected_time_mode_text_ = "delta";
        break;
    }
  }

  const std::string format_label = scan.log_shaped >= scan.screen_shaped ? "candump -l" : "screen output";
  const std::optional<std::int64_t> last_ns =
      scan.have_first_timestamp ? readTailLastTimestampNs(filepath_, kTailBytes) : std::nullopt;

  std::vector<std::string> lines;
  lines.push_back(basenameOf(filepath_));

  std::string format_line = "Format: " + format_label + " \xC2\xB7 " + formatThousands(total_frames) + " frames";
  if (last_ns.has_value()) {
    format_line += " \xC2\xB7 " + formatSecondsOneDecimal(*last_ns - scan.first_timestamp_ns);
  }
  lines.push_back(format_line);

  if (!scan.time_mode.saw_numeric_timestamp) {
    lines.push_back("Start: unknown (no timestamp found).");
  } else if (scan.have_first_timestamp) {
    if (scan.time_mode.mode == TimeMode::kAbsolute) {
      lines.push_back(
          "Start: " + formatDateTime(scan.first_timestamp_ns) +
          " UTC \xC2\xB7 absolute time, lines up with other datasets (e.g. an MCAP)");
    } else {
      lines.push_back(
          "Start: relative time (no wall clock): does not line up with other datasets; "
          "shift it in the Source Timeline.");
    }
  }

  if (scan.malformed > 0) {
    lines.push_back(
        formatThousands(scan.malformed) + " of " + formatThousands(scan.lines_scanned) +
        " line(s) could not be parsed.");
  }
  if (scan.recognized.unsupported() > 0) {
    lines.push_back(
        formatThousands(scan.recognized.unsupported()) + " frame(s) recognized but not decoded (RTR/FD/XL/error).");
  }
  appendCounterLine(lines, &RecognizedCounters::appendWallClock, scan.recognized);
  appendCounterLine(lines, &RecognizedCounters::appendErrorDetail, scan.recognized);
  appendCounterLine(lines, &RecognizedCounters::appendDropCount, scan.recognized);
  if (scan.truncated) {
    lines.push_back(
        "Preview limited to the first " + formatThousands(kScanCap) + " lines; counts above may be incomplete.");
  }

  summary_.clear();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      summary_ += '\n';
    }
    summary_ += lines[i];
  }
}

}  // namespace candump_detail
