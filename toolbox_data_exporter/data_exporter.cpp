#include "data_exporter.hpp"

#include <pj_plugins/sdk/widget_data.hpp>

#include "data_exporter_manifest.hpp"
#include "exporter_dialog_ui.hpp"

std::string DataExporterDialog::manifest() const {
  return kDataExporterManifest;
}

std::string DataExporterDialog::ui_content() const {
  return kDataExporterDialogUi;
}

std::string DataExporterDialog::widget_data() {
  PJ::WidgetData wd;
  return wd.toJson();
}

uint64_t DataExporterToolbox::capabilities() const {
  return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
}

PJ::Status DataExporterToolbox::bind(PJ::sdk::ServiceRegistry services) {
  return ToolboxPluginBase::bind(services);
}
