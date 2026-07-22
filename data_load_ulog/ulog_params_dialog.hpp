#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

namespace ulog_detail {

class ULogParamsDialog : public PJ::DialogPluginTyped {
 public:
  void setFilePath(const std::string& filepath);

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
  std::vector<std::vector<PJ::TableItem>> info_rows_;
  std::vector<std::vector<PJ::TableItem>> param_rows_;
  std::vector<std::vector<PJ::TableItem>> log_rows_;
};

}  // namespace ulog_detail
