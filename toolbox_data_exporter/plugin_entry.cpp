#include "data_exporter.hpp"
#include "data_exporter_manifest.hpp"

PJ_TOOLBOX_PLUGIN(DataExporterToolbox, kDataExporterManifest)
PJ_DIALOG_PLUGIN(DataExporterDialog)

PJ_borrowed_dialog_t DataExporterToolbox::getDialog() {
  prepareDialog();
  return PJ::borrowDialog(dialog_);
}
