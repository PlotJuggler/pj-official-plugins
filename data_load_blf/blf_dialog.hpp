#pragma once

// BlfDialog: assigns a DBC database per CAN channel. Shows which channels the
// file contains (a capped metadata scan) and one DBC file-picker per channel
// (1-4; additional channels can be set through the saved config). The chosen
// databases are round-tripped via onFileSelected + saveConfig.

#include <cstdint>
#include <map>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>

namespace blf_detail {

class BlfDialog : public PJ::DialogPluginTyped {
 public:
  /// Point the dialog at a file; scans it for the set of CAN channels present.
  void setFilePath(const std::string& filepath);

  // --- Dialog protocol ---
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;
  bool onFileSelected(std::string_view widget_name, std::string_view path) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

 private:
  void scanChannels();

  static constexpr int kMaxChannels = 4;

  std::string filepath_;
  std::string summary_;
  std::map<std::uint16_t, std::string> channel_dbc_;  ///< channel -> chosen .dbc path
};

}  // namespace blf_detail
