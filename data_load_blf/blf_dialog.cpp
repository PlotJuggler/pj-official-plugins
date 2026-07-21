#include "blf_dialog.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>

#include "blf_frames.hpp"

// Generated at configure time.
#include "blf_dialog_ui.hpp"
#include "blf_manifest.hpp"

namespace blf_detail {

namespace {
/// Cap the metadata scan so the dialog stays responsive on large files.
constexpr std::uint64_t kScanCap = 200000;
}  // namespace

void BlfDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  scanChannels();
}

void BlfDialog::setChannelDbcs(std::map<std::uint16_t, std::vector<std::string>> channel_dbcs) {
  channel_dbcs_ = std::move(channel_dbcs);
}

std::string BlfDialog::manifest() const {
  return kBlfManifest;
}

std::string BlfDialog::ui_content() const {
  return kBlfDialogUi;
}

std::string BlfDialog::widget_data() {
  PJ::WidgetData wd;
  wd.setText("labelSummary", summary_);
  for (int ch = 1; ch <= kMaxChannels; ++ch) {
    const std::string suffix = std::to_string(ch);
    wd.setFilePicker("buttonDbcCh" + suffix, "Select DBC...", "*.dbc", "Select DBC for CAN channel " + suffix);
    const auto it = channel_dbcs_.find(static_cast<std::uint16_t>(ch));
    std::string label = "(none)";
    if (it != channel_dbcs_.end() && !it->second.empty()) {
      label = it->second.front();
      for (std::size_t k = 1; k < it->second.size(); ++k) {
        label += "; " + it->second[k];
      }
    }
    wd.setText("labelDbcCh" + suffix, label);
  }
  return wd.toJson();
}

std::string BlfDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["filepath"] = filepath_;
  nlohmann::json mapping = nlohmann::json::object();
  for (const auto& [channel, paths] : channel_dbcs_) {
    if (!paths.empty()) {
      mapping[std::to_string(channel)] = paths;
    }
  }
  cfg["channel_dbcs"] = mapping;
  return cfg.dump();
}

bool BlfDialog::loadConfig(std::string_view config_json) {
  const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) {
    return false;
  }
  filepath_ = cfg.value("filepath", std::string{});
  channel_dbcs_.clear();
  if (cfg.contains("channel_dbcs") && cfg["channel_dbcs"].is_object()) {
    for (const auto& [key, value] : cfg["channel_dbcs"].items()) {
      const auto channel = parseChannelKey(key);
      if (!channel || !value.is_array()) {
        continue;  // hand-edited config: skip the bad entry, keep the rest
      }
      auto& paths = channel_dbcs_[*channel];
      for (const auto& entry : value) {
        if (entry.is_string()) {
          paths.push_back(entry.get<std::string>());
        }
      }
    }
  }
  if (!filepath_.empty()) {
    scanChannels();
  }
  return true;
}

bool BlfDialog::onFileSelected(std::string_view widget_name, std::string_view path) {
  constexpr std::string_view kPrefix = "buttonDbcCh";
  if (widget_name.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  const auto channel = parseChannelKey(widget_name.substr(kPrefix.size()));
  if (!channel) {
    return false;
  }
  // The single picker edits the channel as a whole: replace its list.
  channel_dbcs_[*channel] = {std::string(path)};
  return true;  // refresh the UI
}

void BlfDialog::scanChannels() {
  summary_.clear();
  if (filepath_.empty()) {
    return;
  }
  std::map<std::uint16_t, std::uint64_t> counts;
  std::uint64_t seen = 0;
  BlfStats stats;
  (void)readCanFrames(
      filepath_,
      [&](const CanFrame& frame) {
        ++counts[frame.channel];
        return ++seen < kScanCap;
      },
      stats);

  if (counts.empty()) {
    summary_ = "No CAN frames found in file.";
    return;
  }
  const bool capped = seen >= kScanCap;
  summary_ = "CAN channels in file:";
  for (const auto& [channel, count] : counts) {
    summary_ += "  ch" + std::to_string(channel) + " (" + std::to_string(count) + (capped ? "+" : "") + " frames)";
  }
}

}  // namespace blf_detail
