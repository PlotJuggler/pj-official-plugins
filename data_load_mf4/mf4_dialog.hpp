#pragma once

// Display-only preview of the file's metadata (channel groups + channels, no sample data).

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <vector>

namespace mf4_detail {

class Mf4Dialog : public PJ::DialogPluginTyped {
 public:
  /// (Re)parses the file's metadata for the preview.
  void setFilePath(const std::string& filepath);

  /// The dialog has no DBC picker but must carry the configured databases: the host replaces the source config
  /// with this dialog's saveConfig() after accept.
  void setDbcPaths(std::vector<std::string> dbc_paths);

  /// Config state the source reads back (the dialog owns the JSON round-trip).
  const std::string& filePath() const {
    return filepath_;
  }
  // Resolved paths to open (relative entries already joined to the current data
  // file's directory); the saved config stores the portable form instead.
  const std::vector<std::string>& dbcPaths() const {
    return dbc_paths_;
  }

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

 private:
  void parseFile();

  std::string filepath_;
  std::vector<std::string> dbc_paths_;
  std::string summary_;
  std::vector<std::vector<std::string>> group_rows_;
};

}  // namespace mf4_detail
