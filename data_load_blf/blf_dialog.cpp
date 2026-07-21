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
    const auto it = channel_dbc_.find(static_cast<std::uint16_t>(ch));
    wd.setText("labelDbcCh" + suffix, it == channel_dbc_.end() ? std::string("(none)") : it->second);
  }
  return wd.toJson();
}

std::string BlfDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["filepath"] = filepath_;
  nlohmann::json mapping = nlohmann::json::object();
  for (const auto& [channel, path] : channel_dbc_) {
    if (!path.empty()) {
      mapping[std::to_string(channel)] = nlohmann::json::array({path});
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
  channel_dbc_.clear();
  if (cfg.contains("channel_dbcs") && cfg["channel_dbcs"].is_object()) {
    for (const auto& [key, value] : cfg["channel_dbcs"].items()) {
      if (value.is_array() && !value.empty() && value.front().is_string()) {
        channel_dbc_[static_cast<std::uint16_t>(std::stoul(key))] = value.front().get<std::string>();
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
  const auto channel = static_cast<std::uint16_t>(std::stoul(std::string(widget_name.substr(kPrefix.size()))));
  channel_dbc_[channel] = std::string(path);
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
