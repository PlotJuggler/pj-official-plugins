#pragma once

// Mf4Dialog: a preview dialog for the MF4/MDF data source. It parses the file's
// metadata (channel groups + channels, no sample data) and displays it as a
// table so the user can see what will be imported. v1 is display-only;
// interactive channel selection and DBC-file management are future work (they
// require round-tripping widget input through the dialog engine).

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <vector>

namespace mf4_detail {

class Mf4Dialog : public PJ::DialogPluginTyped {
 public:
  /// Point the dialog at a file; (re)parses its metadata for the preview.
  void setFilePath(const std::string& filepath);

  /// DBC databases configured on the source. The dialog does not edit them
  /// (v1 has no DBC picker) but must carry them: the host replaces the source
  /// config with this dialog's saveConfig() after accept.
  void setDbcPaths(std::vector<std::string> dbc_paths);

  /// Config state the source reads back (the dialog owns the JSON round-trip).
  const std::string& filePath() const {
    return filepath_;
  }
  const std::vector<std::string>& dbcPaths() const {
    return dbc_paths_;
  }

  // --- Dialog protocol ---
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
