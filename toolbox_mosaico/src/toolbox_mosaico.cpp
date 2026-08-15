// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ToolboxMosaico — PJ4 port of the PJ3 plugin that browses and imports
// data from Mosaico cloud servers via Apache Arrow Flight. Embedded Lua
// (sol2) metadata filter, multi-topic streaming, time-range selection.

#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>

#include "mosaico_dialog.hpp"
#include "mosaico_panel_manifest.hpp"

namespace mosaico {

class MosaicoToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    // Once the toolbox host is bound, hand a host-view provider to the
    // dialog so the worker can ingest Arrow data on fetch completion.
    dialog_.setHostProvider([this]() { return toolboxHost(); });
    // Runtime host carries notifyDataChanged — the dialog calls this after
    // a successful import so the app flushes pending writer chunks and
    // rebuilds the catalog tree. Without it, ingested data stays buffered
    // in the writer and the dataset panel never sees the new topics.
    dialog_.setRuntimeHostProvider([this]() { return runtimeHost(); });
    // Optional pj.source_promotion.v1: with it, a completed Download is
    // recorded into a session cache artifact and promoted to a file-backed
    // dataset, so a saved layout can re-import it. Absent service = every
    // fetch stays eager-only (previous behavior).
    promotion_host_ = services.get<PJ::sdk::SourcePromotionHostService>().value_or(PJ::SourcePromotionHostView{});
    dialog_.setPromotionProvider([this]() { return promotion_host_; });
    // Bind the optional pj.settings.v1 store (QSettings-like persistence) and
    // restore persisted UI state + auto-connect. setSettings() also runs when
    // the service is absent — an unbound view just reads defaults.
    dialog_.setSettings(services.get<PJ::sdk::SettingsStoreService>().value_or(PJ::sdk::SettingsView{}));
    return PJ::okStatus();
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  MosaicoDialog dialog_;
  PJ::SourcePromotionHostView promotion_host_{};
};

}  // namespace mosaico

#ifdef PJ_TOOLBOX_PLUGIN_NAMED
PJ_TOOLBOX_PLUGIN_NAMED(mosaico::MosaicoToolbox, MosaicoToolbox, kMosaicoPanelManifest)
PJ_DIALOG_PLUGIN_NAMED(mosaico::MosaicoDialog, MosaicoDialog, kMosaicoPanelManifest)
#else
PJ_TOOLBOX_PLUGIN(mosaico::MosaicoToolbox, kMosaicoPanelManifest)
PJ_DIALOG_PLUGIN(mosaico::MosaicoDialog, kMosaicoPanelManifest)
#endif
