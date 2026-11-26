#include "candump_dialog.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/time_format.hpp>
#include <pj_can_dbc/dictionary_paths.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_utf8_path/utf8_path.hpp>
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

/// File name (no folder) of the capture the forced time mode was chosen for.
constexpr char kTimeModeFileKey[] = "time_mode_file";

/// Per-interface protocol: {"can0": "j1939"}; an interface without an entry is raw CAN.
constexpr char kProtocolsKey[] = "iface_protocols";

/// comboProtocol items, in pj_can_dbc::CanProtocol order, and their config names.
struct ProtocolName {
  pj_can_dbc::CanProtocol protocol;
  const char* label;
  const char* config;
};
constexpr ProtocolName kProtocols[] = {
    {pj_can_dbc::CanProtocol::kRaw, "Raw CAN", "raw"},
    {pj_can_dbc::CanProtocol::kJ1939, "J1939", "j1939"},
    {pj_can_dbc::CanProtocol::kNmea2000, "NMEA 2000", "nmea2000"},
};

const ProtocolName& protocolName(pj_can_dbc::CanProtocol protocol) {
  for (const auto& name : kProtocols) {
    if (name.protocol == protocol) {
      return name;
    }
  }
  return kProtocols[0];
}

/// "," thousands separators (16000 -> "16,000"); no locale dependence.
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

/// A non-negative nanosecond duration as "<seconds>.<decisecond> s" (e.g. "40.0 s"), in integer arithmetic.
std::string formatSecondsOneDecimal(std::int64_t duration_ns) {
  const std::uint64_t mag = duration_ns > 0 ? static_cast<std::uint64_t>(duration_ns) : 0;
  const std::uint64_t whole = mag / 1'000'000'000ull;
  const std::uint64_t tenths = (mag % 1'000'000'000ull) / 100'000'000ull;
  return formatThousands(whole) + "." + std::to_string(tenths) + " s";
}

/// UTC "YYYY-MM-DD HH:MM:SS": PJ::formatIso8601Utc (no locale, like the rest of the host) with a space for "T".
std::string formatDateTime(std::int64_t ts_ns) {
  std::string iso = PJ::formatIso8601Utc(ts_ns);
  if (iso.size() > 10) {
    iso[10] = ' ';
  }
  return iso;
}

/// Text after the last '/' or '\', or the whole string if neither is present.
std::string basenameOf(const std::string& path) {
  const std::size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

/// Runs one of RecognizedCounters' `append*` helpers, which write "; <text>" as a continuation of
/// CandumpSource's one-line summary, and appends the text as its own line with the "; " stripped: same wording,
/// one warning per line in the preview.
void appendCounterLine(
    std::vector<std::string>& lines, void (RecognizedCounters::*appender)(std::string&) const,
    const RecognizedCounters& counters) {
  std::string fragment;
  (counters.*appender)(fragment);
  if (!fragment.empty()) {
    lines.push_back(fragment.size() > 2 ? fragment.substr(2) : fragment);
  }
}

/// Raw timestamp of the last line carrying a numeric one, found by scanning the last `tail_bytes` of `path`
/// backwards. Independent of the forward prescan, whose 200k-line cap can stop long before the end of a large
/// file.
std::optional<std::int64_t> readTailLastTimestampNs(const std::string& path, std::uint64_t tail_bytes) {
  std::ifstream file(PJ::utf8_path::pathFromUtf8(path), std::ios::binary);
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
    if (hasNumericTimestamp(parsed) && parsed.kind != LineKind::kMalformed) {
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

pj_can_dbc::CanProtocol CandumpDialog::protocolOf(const std::string& iface) const {
  const auto it = iface_protocols_.find(iface);
  return it == iface_protocols_.end() ? pj_can_dbc::CanProtocol::kRaw : it->second;
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
  wd.setTableHeaders("tableInterfaces", {"Interface", "Frames", "Messages", "Dictionary", "Protocol"});
  constexpr std::size_t kDictionaryColumn = 3;
  auto rows = interface_rows_;
  std::vector<std::string> dict_paths(rows.size());
  for (std::size_t row = 0; row < rows.size(); ++row) {
    if (const auto it = iface_dicts_.find(interface_names_[row]); it != iface_dicts_.end() && !it->second.empty()) {
      dict_paths[row] = it->second.front();
    }
    rows[row][kDictionaryColumn] = dict_paths[row].empty() ? "none" : basenameOf(dict_paths[row]);
    rows[row][kDictionaryColumn + 1] = protocolName(protocolOf(interface_names_[row])).label;
  }
  wd.setTableRows("tableInterfaces", rows);
  for (std::size_t row = 0; row < dict_paths.size(); ++row) {
    if (!dict_paths[row].empty()) {
      wd.setCellTooltip("tableInterfaces", static_cast<int>(row), static_cast<int>(kDictionaryColumn), dict_paths[row]);
    }
  }

  // The dictionary buttons act on the selected table row (the interface).
  if (!interface_names_.empty()) {
    wd.setSelectedRows("tableInterfaces", {selected_interface_index_});
  }
  wd.setFilePicker("buttonDictionary", "Choose .dbc / .csv...", "*.dbc *.csv", "Select a .dbc or CSV signal table");
  const std::string iface = currentInterface();
  wd.setEnabled("buttonDictionary", !iface.empty());
  wd.setEnabled("buttonClearDictionary", !iface.empty());
  std::vector<std::string> protocol_labels;
  for (const auto& name : kProtocols) {
    protocol_labels.emplace_back(name.label);
  }
  wd.setItems("comboProtocol", protocol_labels);
  wd.setCurrentIndex("comboProtocol", static_cast<int>(protocolOf(iface)));
  wd.setEnabled("comboProtocol", !iface.empty());

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
      mapping[iface] = pj_can_dbc::toConfigPaths(paths, filepath_);
    }
  }
  cfg["iface_dicts"] = mapping;
  if (const auto base = pj_can_dbc::auxBaseDirToSave(filepath_, !mapping.empty())) {
    cfg[pj_can_dbc::kAuxBaseDirKey] = *base;
  }
  if (!iface_protocols_.empty()) {
    nlohmann::json protocols = nlohmann::json::object();
    for (const auto& [iface, protocol] : iface_protocols_) {
      protocols[iface] = protocolName(protocol).config;
    }
    cfg[kProtocolsKey] = protocols;
  }
  cfg["raw_unassigned"] = raw_unassigned_;
  cfg["time_mode"] = time_mode_override_;
  if (time_mode_override_ != 0) {
    cfg[kTimeModeFileKey] = basenameOf(filepath_);
  }
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
  const pj_can_dbc::AuxPathContext context{
      filepath_, cfg.value(pj_can_dbc::kSavedFilepathKey, std::string{}),
      cfg.value(pj_can_dbc::kAuxBaseDirKey, std::string{})};
  raw_unassigned_ = cfg.value("raw_unassigned", true);
  time_mode_override_ = cfg.value("time_mode", 0);
  if (time_mode_override_ < 0 || time_mode_override_ > 3) {
    time_mode_override_ = 0;
  }
  // A forced mode belongs to the capture it was chosen for: the host pre-fills the next file's dialog with this
  // config (filepath rewritten), and that file needs its own auto-detection. Captures are told apart by file name,
  // not path, since a layout replay may find the capture in another folder (a moved layout, or a browser upload
  // under a fresh backing path with no __pj_saved_filepath). A config without the key keeps its mode.
  if (const auto chosen_for = cfg.value(kTimeModeFileKey, std::string{});
      !chosen_for.empty() && basenameOf(chosen_for) != basenameOf(filepath_)) {
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
      paths = pj_can_dbc::resolveAuxPaths(paths, context);
    }
  }
  iface_protocols_.clear();
  if (cfg.contains(kProtocolsKey) && cfg[kProtocolsKey].is_object()) {
    for (const auto& [key, value] : cfg[kProtocolsKey].items()) {
      if (!isValidInterfaceKey(key) || !value.is_string()) {
        continue;  // hand-edited config: skip the bad entry, keep the rest
      }
      for (const auto& name : kProtocols) {
        if (value.get<std::string>() == name.config && name.protocol != pj_can_dbc::CanProtocol::kRaw) {
          iface_protocols_[key] = name.protocol;
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

bool CandumpDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "tableInterfaces" || selected.empty()) {
    return false;
  }
  // The host reports each selected row by its first-column text: the interface name.
  for (std::size_t i = 0; i < interface_names_.size(); ++i) {
    if (interface_names_[i] == selected.front()) {
      selected_interface_index_ = static_cast<int>(i);
      return true;
    }
  }
  return false;
}

bool CandumpDialog::onIndexChanged(std::string_view widget_name, int index) {
  if (widget_name == "comboTimeMode") {
    time_mode_override_ = index;
    return true;
  }
  if (widget_name == "comboProtocol") {
    const std::string iface = currentInterface();
    if (iface.empty() || index < 0 || index >= static_cast<int>(std::size(kProtocols))) {
      return false;
    }
    const auto protocol = kProtocols[static_cast<std::size_t>(index)].protocol;
    if (protocol == pj_can_dbc::CanProtocol::kRaw) {
      iface_protocols_.erase(iface);
    } else {
      iface_protocols_[iface] = protocol;
    }
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

  std::ifstream file(PJ::utf8_path::pathFromUtf8(filepath_));
  if (!file.is_open()) {
    summary_ = "Failed to open file.";
    return;
  }

  // Bounded prescan: interface/grammar/malformed counts and the TimeMode decision, shared with
  // CandumpSource::importData().
  const PrescanResult scan = prescanCandump(file, kScanCap);

  std::uint64_t total_frames = 0;
  interface_names_.reserve(scan.interfaces.size());
  interface_rows_.reserve(scan.interfaces.size());
  for (const auto& iface : scan.interfaces) {
    total_frames += iface.frames;
    interface_names_.push_back(iface.interface);
    // The Dictionary and Protocol columns (3, 4) follow live edits, so widget_data() fills them per render.
    interface_rows_.push_back(
        {iface.interface, formatThousands(iface.frames) + (scan.truncated ? "+" : ""),
         formatThousands(iface.distinct_ids), std::string{}, std::string{}});
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
