#include "candump_dialog.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
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

/// Renders raw nanoseconds (seconds*1e9 + fraction, may be negative) as
/// "seconds.nanoseconds" using integer arithmetic only -- consistent with
/// the parser's no-double philosophy, and avoids locale-dependent formatting
/// in a summary label shown to the user.
std::string formatNs(std::int64_t ns) {
  const bool neg = ns < 0;
  std::uint64_t mag = neg ? static_cast<std::uint64_t>(-ns) : static_cast<std::uint64_t>(ns);
  const std::uint64_t seconds = mag / 1'000'000'000ull;
  const std::uint64_t frac = mag % 1'000'000'000ull;
  std::string frac_text = std::to_string(frac);
  frac_text.insert(0, 9 - frac_text.size(), '0');
  return (neg ? std::string("-") : std::string()) + std::to_string(seconds) + "." + frac_text;
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
  wd.setTableHeaders("tableInterfaces", {"Interface", "Frames", "IDs", "Dictionary"});
  wd.setTableRows("tableInterfaces", interface_rows_);

  wd.setItems("comboInterface", interface_names_);
  wd.setCurrentIndex("comboInterface", selected_interface_index_);

  wd.setFilePicker("buttonDictionary", "Select dictionary...", "*.dbc *.csv", "Select DBC or ARUS-CSV dictionary");
  const std::string iface = currentInterface();
  std::string dict_label = "(none)";
  if (const auto it = iface_dicts_.find(iface); it != iface_dicts_.end() && !it->second.empty()) {
    dict_label = it->second.front();
    for (std::size_t k = 1; k < it->second.size(); ++k) {
      dict_label += "; " + it->second[k];
    }
  }
  wd.setText("labelDictionary", dict_label);
  wd.setEnabled("buttonDictionary", !iface.empty());
  wd.setEnabled("buttonClearDictionary", !iface.empty());

  wd.setChecked("checkRawUnassigned", raw_unassigned_);

  wd.setItems(
      "comboTimeMode",
      {"Auto-detect", "Force absolute", "Force relative (monotonic, -tz)", "Force relative (delta, -td)"});
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

  // First-assigned-dictionary label for the table's "Dictionary" column
  // (labelDictionary in widget_data() below shows ALL assigned paths for
  // the currently-selected interface instead -- a different label, kept
  // separate; this one lambda covers every row here).
  const auto dictLabelFor = [this](const std::string& iface) -> std::string {
    const auto it = iface_dicts_.find(iface);
    if (it == iface_dicts_.end() || it->second.empty()) {
      return "(none)";
    }
    return it->second.front();
  };

  interface_names_.reserve(scan.interfaces.size());
  interface_rows_.reserve(scan.interfaces.size());
  for (const auto& iface : scan.interfaces) {
    interface_names_.push_back(iface.interface);
    interface_rows_.push_back(
        {iface.interface, std::to_string(iface.frames) + (scan.truncated ? "+" : ""),
         std::to_string(iface.distinct_ids), dictLabelFor(iface.interface)});
  }

  if (scan.lines_scanned == 0) {
    summary_ = "Empty file.";
    return;
  }
  if (scan.malformed == scan.lines_scanned) {
    summary_ = "This does not look like a candump capture (record with `candump -l` or `-t z/d/a`).";
    return;
  }

  const std::string format_label = scan.log_shaped >= scan.screen_shaped ? "log format (candump -l)" : "screen format";
  std::string mode_label = "no timestamp found";
  if (scan.time_mode.saw_numeric_timestamp) {
    switch (scan.time_mode.mode) {
      case TimeMode::kAbsolute:
        mode_label = "absolute";
        break;
      case TimeMode::kRelativeMonotonic:
        mode_label = "relative, monotonic (-tz)";
        break;
      case TimeMode::kRelativeDelta:
        mode_label =
            "relative, delta (-td); frames will not align with other absolute-time sources without manual offset";
        break;
    }
  } else if (scan.recognized.wall_clock > 0) {
    mode_label = "wall-clock (-tA) -- unsupported in this version";
  }

  summary_ =
      format_label + "; time mode: " + mode_label + "; " + std::to_string(scan.interfaces.size()) + " interface(s)";
  if (scan.malformed > 0) {
    summary_ += "; " + std::to_string(scan.malformed) + "/" + std::to_string(scan.lines_scanned) + " malformed line(s)";
  }
  if (scan.recognized.unsupported() > 0) {
    summary_ +=
        "; " + std::to_string(scan.recognized.unsupported()) + " recognized-but-unsupported frame(s) (RTR/FD/XL/error)";
  }
  scan.recognized.appendWallClock(summary_);
  scan.recognized.appendErrorDetail(summary_);
  scan.recognized.appendDropCount(summary_);
  if (scan.truncated) {
    summary_ += "; prescan capped at " + std::to_string(kScanCap) + " lines";
  }
  if (scan.have_first_timestamp) {
    summary_ += "; range starts at " + formatNs(scan.first_timestamp_ns);
    if (const auto last_ns = readTailLastTimestampNs(filepath_, kTailBytes)) {
      summary_ += ", ends at " + formatNs(*last_ns);
    }
  }
}

}  // namespace candump_detail
