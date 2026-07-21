#pragma once

// BlfDialog: assigns a DBC database per CAN channel. Shows which channels the
// file contains (a capped metadata scan) and one DBC file-picker per channel
// (1-4; additional channels can be set through the saved config). The chosen
// databases are round-tripped via onFileSelected + saveConfig.

#include <cstdint>
#include <map>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <vector>

namespace blf_detail {

class BlfDialog : public PJ::DialogPluginTyped {
 public:
  /// Point the dialog at a file; scans it for the set of CAN channels present.
  void setFilePath(const std::string& filepath);

  /// Per-channel DBC lists configured on the source. The single picker per
  /// channel edits a channel's list as a whole, but an untouched channel must
  /// round-trip unchanged: the host replaces the source config with this
  /// dialog's saveConfig() after accept.
  void setChannelDbcs(std::map<std::uint16_t, std::vector<std::string>> channel_dbcs);

  /// Config state the source reads back (the dialog owns the JSON round-trip).
  const std::string& filePath() const {
    return filepath_;
  }
  const std::map<std::uint16_t, std::vector<std::string>>& channelDbcs() const {
    return channel_dbcs_;
  }

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
  std::map<std::uint16_t, std::vector<std::string>> channel_dbcs_;  ///< channel -> .dbc paths
};

}  // namespace blf_detail
