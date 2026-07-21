#pragma once

#include <cstdint>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>

class DataExporterDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
};

namespace PJ {

template <>
PJ_borrowed_dialog_t borrowDialog<::DataExporterDialog>(::DataExporterDialog& dialog) noexcept;

}  // namespace PJ

class DataExporterToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override;
  PJ_borrowed_dialog_t getDialog() override;
  PJ::Status bind(PJ::sdk::ServiceRegistry services) override;

 private:
  DataExporterDialog dialog_;
};
