#include "can_dialog.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <utility>

// Generated at configure time.
#include "can_stream_manifest.hpp"
#include "datastream_can_ui.hpp"

namespace can_stream {

namespace {

using json = nlohmann::json;

struct Preset {
  int value;
  const char* label;
};

constexpr std::array<Preset, 5> kBitrates{{
    {0, "Keep device setting"},
    {125000, "125 kbit/s"},
    {250000, "250 kbit/s"},
    {500000, "500 kbit/s"},
    {1000000, "1 Mbit/s"},
}};

constexpr std::array<Preset, 5> kDataBitrates{{
    {0, "Keep device setting"},
    {2000000, "2 Mbit/s"},
    {4000000, "4 Mbit/s"},
    {5000000, "5 Mbit/s"},
    {8000000, "8 Mbit/s"},
}};

constexpr std::array<const char*, 2> kTimestampKeys{"device", "host"};

/// The values a bitrate combo offers: the presets, plus `current` when a saved config holds another rate
/// (so it is shown and survives a save).
template <std::size_t N>
std::vector<int> choices(const std::array<Preset, N>& presets, int current) {
  std::vector<int> values;
  for (const auto& preset : presets) {
    values.push_back(preset.value);
  }
  if (std::find(values.begin(), values.end(), current) == values.end()) {
    values.push_back(current);
  }
  return values;
}

template <std::size_t N>
void writeBitrateCombo(PJ::WidgetData& wd, std::string_view name, const std::array<Preset, N>& presets, int current) {
  const auto values = choices(presets, current);
  std::vector<std::string> labels;
  for (std::size_t i = 0; i < values.size(); ++i) {
    labels.push_back(i < presets.size() ? presets[i].label : std::to_string(values[i]) + " bit/s");
  }
  wd.setItems(name, labels);
  const auto it = std::find(values.begin(), values.end(), current);
  wd.setCurrentIndex(name, static_cast<int>(it - values.begin()));
}

template <std::size_t N>
void pickBitrate(const std::array<Preset, N>& presets, int index, int& bitrate) {
  const auto values = choices(presets, bitrate);
  if (index >= 0 && static_cast<std::size_t>(index) < values.size()) {
    bitrate = values[static_cast<std::size_t>(index)];
  }
}

// Typed reads of a hand-editable config: a missing or wrongly-typed key keeps `fallback`.
std::string stringOr(const json& cfg, const char* key, std::string fallback) {
  const auto it = cfg.find(key);
  return it != cfg.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}
bool boolOr(const json& cfg, const char* key, bool fallback) {
  const auto it = cfg.find(key);
  return it != cfg.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
int bitrateOr(const json& cfg, const char* key, int fallback) {
  const auto it = cfg.find(key);
  if (it == cfg.end() || !it->is_number_integer()) {
    return fallback;
  }
  const auto value = it->get<std::int64_t>();
  return value >= 0 && value <= 100'000'000 ? static_cast<int>(value) : fallback;
}

}  // namespace

CanDialog::CanDialog(ListBackends list_backends, ListInterfaces list_interfaces)
    : list_backends_(std::move(list_backends)), list_interfaces_(std::move(list_interfaces)) {}

std::string CanDialog::manifest() const {
  return kCanStreamManifest;
}

std::string CanDialog::ui_content() const {
  return kDataStreamCanUi;
}

void CanDialog::refreshBackends() {
  backends_ = list_backends_();
  if (settings_.backend.empty() && !backends_.empty()) {
    settings_.backend = backends_.front();
  }
  if (!settings_.backend.empty() &&
      std::find(backends_.begin(), backends_.end(), settings_.backend) == backends_.end()) {
    backends_.push_back(settings_.backend);  // saved on another machine: keep it, the start reports it
  }
}

void CanDialog::refreshInterfaces() {
  interfaces_.clear();
  interfaces_error_.clear();
  if (settings_.backend.empty()) {
    return;
  }
  auto listed = list_interfaces_(settings_.backend);
  if (listed) {
    interfaces_ = std::move(*listed);
  } else {
    interfaces_error_ = listed.error();
  }
}

std::string CanDialog::widget_data() {
  if (!lists_loaded_) {
    refreshBackends();
    refreshInterfaces();
    lists_loaded_ = true;
  }

  PJ::WidgetData wd;
  wd.setItems("comboBackend", backends_);
  const auto backend_it = std::find(backends_.begin(), backends_.end(), settings_.backend);
  wd.setCurrentIndex(
      "comboBackend", backend_it == backends_.end() ? -1 : static_cast<int>(backend_it - backends_.begin()));

  wd.setItems("comboInterface", interfaces_);
  wd.setText("comboInterface", settings_.interface_name);
  if (!interfaces_error_.empty()) {
    wd.setText("labelStatus", interfaces_error_);
  } else if (interfaces_.empty() && !settings_.backend.empty()) {
    wd.setText("labelStatus", "No " + settings_.backend + " interface detected: type its name.");
  } else {
    wd.setText("labelStatus", "");
  }

  writeBitrateCombo(wd, "comboBitrate", kBitrates, settings_.bitrate);
  wd.setChecked("checkCanFd", settings_.can_fd);
  writeBitrateCombo(wd, "comboDataBitrate", kDataBitrates, settings_.data_bitrate);
  wd.setEnabled("comboDataBitrate", settings_.can_fd);
  wd.setItems("comboTimestamps", {"Adapter clock", "Arrival time"});
  wd.setCurrentIndex("comboTimestamps", settings_.timestamps == TimestampSource::kHost ? 1 : 0);

  wd.setListItems("listDictionaries", dictionaries_);
  const bool has_selection =
      std::find(dictionaries_.begin(), dictionaries_.end(), selected_dictionary_) != dictionaries_.end();
  if (has_selection) {
    wd.setSelectedItems("listDictionaries", {selected_dictionary_});
  }
  wd.setFilePicker("buttonAddDictionary", "Add .dbc / .csv...", "*.dbc *.csv", "Select a .dbc or CSV signal table");
  wd.setEnabled("buttonRemoveDictionary", has_selection);
  wd.setChecked("checkRawUnassigned", raw_unassigned_);

  wd.setOkEnabled(!settings_.backend.empty() && !settings_.interface_name.empty());
  return wd.toJson();
}

bool CanDialog::loadConfig(std::string_view config_json) {
  const auto cfg = json::parse(config_json, nullptr, false);
  if (cfg.is_discarded() || !cfg.is_object()) {
    return false;
  }
  settings_ = BusSettings{};
  settings_.backend = stringOr(cfg, "backend", "");
  settings_.interface_name = stringOr(cfg, "interface", "");
  settings_.bitrate = bitrateOr(cfg, "bitrate", 0);
  settings_.can_fd = boolOr(cfg, "can_fd", false);
  settings_.data_bitrate = bitrateOr(cfg, "data_bitrate", 0);
  settings_.timestamps =
      stringOr(cfg, "timestamps", "device") == "host" ? TimestampSource::kHost : TimestampSource::kDevice;
  dictionaries_.clear();
  if (const auto it = cfg.find("dictionaries"); it != cfg.end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (entry.is_string() && !entry.get<std::string>().empty()) {
        dictionaries_.push_back(entry.get<std::string>());
      }
    }
  }
  raw_unassigned_ = boolOr(cfg, "raw_unassigned", true);
  selected_dictionary_.clear();
  lists_loaded_ = false;  // re-list: the saved backend may not be installed here
  return true;
}

std::string CanDialog::saveConfig() const {
  json cfg;
  cfg["backend"] = settings_.backend;
  cfg["interface"] = settings_.interface_name;
  cfg["bitrate"] = settings_.bitrate;
  cfg["can_fd"] = settings_.can_fd;
  cfg["data_bitrate"] = settings_.data_bitrate;
  cfg["timestamps"] = kTimestampKeys[settings_.timestamps == TimestampSource::kHost ? 1 : 0];
  cfg["dictionaries"] = dictionaries_;
  cfg["raw_unassigned"] = raw_unassigned_;
  return cfg.dump();
}

bool CanDialog::onIndexChanged(std::string_view widget_name, int index) {
  if (widget_name == "comboBackend") {
    if (index < 0 || static_cast<std::size_t>(index) >= backends_.size() ||
        backends_[static_cast<std::size_t>(index)] == settings_.backend) {
      return false;
    }
    settings_.backend = backends_[static_cast<std::size_t>(index)];
    refreshInterfaces();
    // The old backend's interface name rarely means anything to the new one: preselect a listed one.
    if (std::find(interfaces_.begin(), interfaces_.end(), settings_.interface_name) == interfaces_.end()) {
      settings_.interface_name = interfaces_.empty() ? std::string() : interfaces_.front();
    }
    return true;
  }
  if (widget_name == "comboBitrate") {
    pickBitrate(kBitrates, index, settings_.bitrate);
    return true;
  }
  if (widget_name == "comboDataBitrate") {
    pickBitrate(kDataBitrates, index, settings_.data_bitrate);
    return true;
  }
  if (widget_name == "comboTimestamps") {
    settings_.timestamps = index == 1 ? TimestampSource::kHost : TimestampSource::kDevice;
    return true;
  }
  return false;
}

bool CanDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  if (widget_name != "comboInterface") {
    return false;
  }
  settings_.interface_name = std::string(text);
  return true;  // OK follows whether an interface is set
}

bool CanDialog::onToggled(std::string_view widget_name, bool checked) {
  if (widget_name == "checkCanFd") {
    settings_.can_fd = checked;
    return true;
  }
  if (widget_name == "checkRawUnassigned") {
    raw_unassigned_ = checked;
    return true;
  }
  return false;
}

bool CanDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "buttonRefresh") {
    refreshBackends();
    refreshInterfaces();
    lists_loaded_ = true;
    return true;
  }
  if (widget_name == "buttonRemoveDictionary") {
    const auto it = std::find(dictionaries_.begin(), dictionaries_.end(), selected_dictionary_);
    if (it == dictionaries_.end()) {
      return false;
    }
    dictionaries_.erase(it);
    selected_dictionary_.clear();
    return true;
  }
  return false;
}

bool CanDialog::onFileSelected(std::string_view widget_name, std::string_view path) {
  if (widget_name != "buttonAddDictionary" || path.empty()) {
    return false;
  }
  if (std::find(dictionaries_.begin(), dictionaries_.end(), path) == dictionaries_.end()) {
    dictionaries_.emplace_back(path);
  }
  return true;
}

bool CanDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "listDictionaries") {
    return false;
  }
  selected_dictionary_ = selected.empty() ? std::string() : selected.front();
  return true;  // the remove button follows the selection
}

}  // namespace can_stream
