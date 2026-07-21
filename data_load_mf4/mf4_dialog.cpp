#include "mf4_dialog.hpp"

#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "mf4_reader.hpp"

// Generated at configure time.
#include "mf4_dialog_ui.hpp"
#include "mf4_manifest.hpp"

namespace mf4_detail {

namespace {
const char* busName(int bus_type) {
  switch (bus_type) {
    case 0:
      return "-";
    case 1:
      return "Other";
    case 2:
      return "CAN";
    case 3:
      return "LIN";
    case 4:
      return "MOST";
    case 5:
      return "FlexRay";
    case 6:
      return "Ethernet";
    default:
      return "?";
  }
}
}  // namespace

void Mf4Dialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  parseFile();
}

void Mf4Dialog::setDbcPaths(std::vector<std::string> dbc_paths) {
  dbc_paths_ = std::move(dbc_paths);
}

std::string Mf4Dialog::manifest() const {
  return kMf4Manifest;
}

std::string Mf4Dialog::ui_content() const {
  return kMf4DialogUi;
}

std::string Mf4Dialog::widget_data() {
  PJ::WidgetData wd;
  wd.setText("labelSummary", summary_);
  wd.setTableHeaders("tableGroups", {"Channel Group", "Samples", "Channels", "Bus"});
  wd.setTableRows("tableGroups", group_rows_);
  return wd.toJson();
}

std::string Mf4Dialog::saveConfig() const {
  return nlohmann::json{{"filepath", filepath_}, {"dbc_paths", dbc_paths_}}.dump();
}

bool Mf4Dialog::loadConfig(std::string_view config_json) {
  const auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) {
    return false;
  }
  filepath_ = cfg.value("filepath", std::string{});
  dbc_paths_.clear();
  if (cfg.contains("dbc_paths") && cfg["dbc_paths"].is_array()) {
    for (const auto& entry : cfg["dbc_paths"]) {
      if (entry.is_string()) {
        dbc_paths_.push_back(entry.get<std::string>());
      }
    }
  }
  if (!filepath_.empty()) {
    parseFile();
  }
  return true;
}

void Mf4Dialog::parseFile() {
  group_rows_.clear();
  summary_.clear();
  if (filepath_.empty()) {
    return;
  }

  Mf4Reader reader;
  if (auto status = reader.open(filepath_); !status) {
    summary_ = "Failed to open file: " + status.error();
    return;
  }

  const auto& groups = reader.groups();
  std::size_t total_channels = 0;
  for (const auto& group : groups) {
    group_rows_.push_back(
        {group.name.empty() ? std::string("(unnamed)") : group.name, std::to_string(group.sample_count),
         std::to_string(group.channels.size()), busName(group.bus_type)});
    total_channels += group.channels.size();
  }
  summary_ = std::to_string(groups.size()) + " channel group(s), " + std::to_string(total_channels) + " channel(s)";
  if (!reader.finalized()) {
    summary_ += "  —  file is not finalized";
  }
}

}  // namespace mf4_detail
