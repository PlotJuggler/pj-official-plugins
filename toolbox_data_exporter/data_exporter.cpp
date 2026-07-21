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

namespace PJ {

template <>
PJ_borrowed_dialog_t borrowDialog<::DataExporterDialog>(::DataExporterDialog& dialog) noexcept {
  const auto* vtable = DialogPluginBase::vtableWithCreate([]() noexcept -> void* {
    try {
      return static_cast<DialogPluginBase*>(new DataExporterDialog());
    } catch (...) {
      return nullptr;
    }
  });
  return PJ_borrowed_dialog_t{static_cast<DialogPluginBase*>(&dialog), vtable};
}

}  // namespace PJ

uint64_t DataExporterToolbox::capabilities() const {
  return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
}

PJ_borrowed_dialog_t DataExporterToolbox::getDialog() {
  return PJ::borrowDialog(dialog_);
}

PJ::Status DataExporterToolbox::bind(PJ::sdk::ServiceRegistry services) {
  return ToolboxPluginBase::bind(services);
}
