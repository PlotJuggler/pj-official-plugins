// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ToolboxMosaico — PJ4 port of the PJ3 plugin that browses and imports
// data from Mosaico cloud servers via Apache Arrow Flight. Embedded Lua
// (sol2) metadata filter, multi-topic streaming, time-range selection.

#include <pj_base/descriptor_import_protocol.h>

#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>

#include "descriptor_import_provider.hpp"
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
    const auto settings = services.get<PJ::sdk::SettingsStoreService>().value_or(PJ::sdk::SettingsView{});
    dialog_.setSettings(settings);
    // The descriptor-import provider (pj.descriptor_import.v1): same settings
    // view as the dialog (headless credential + cache-directory resolution at
    // import time, main-thread), plus the host providers its per-job pulls
    // ingest through. Network-free at bind — the provider touches the network
    // only inside an authorized start_import.
    provider_.bind(
        settings,
        {[this]() { return toolboxHost(); }, [this]() { return runtimeHost(); }, [this]() { return promotion_host_; }});
    return PJ::okStatus();
  }

  const void* pluginExtension(std::string_view id) override {
    if (id == PJ_DESCRIPTOR_IMPORT_EXTENSION_V1) {
      return &descriptor_import_ext_;
    }
    return nullptr;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  // plugin_ctx is the toolbox instance the host called get_plugin_extension
  // with (the PJ_TOOLBOX_PLUGIN create_fn returns `new MosaicoToolbox()` as
  // void*, so the casts below are exact). The provider walls exceptions
  // itself.
  static bool descriptorQueryThunk(
      void* plugin_ctx, PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out_result,
      PJ_error_t* out_error) noexcept {
    if (plugin_ctx == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "null plugin_ctx");
      return false;
    }
    return static_cast<MosaicoToolbox*>(plugin_ctx)->provider_.queryDescriptor(descriptor_json, out_result, out_error);
  }

  static bool descriptorStartThunk(
      void* plugin_ctx, const PJ_descriptor_import_start_request_v1_t* request,
      const PJ_descriptor_import_callbacks_v1_t* callbacks, void* callback_ctx, PJ_joinable_job_t* out_job,
      PJ_error_t* out_error) noexcept {
    if (plugin_ctx == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "null plugin_ctx");
      return false;
    }
    return static_cast<MosaicoToolbox*>(plugin_ctx)
        ->provider_.startImport(request, callbacks, callback_ctx, out_job, out_error);
  }

  MosaicoDialog dialog_;
  PJ::SourcePromotionHostView promotion_host_{};
  DescriptorImportProvider provider_;
  PJ_descriptor_import_provider_v1_t descriptor_import_ext_{
      sizeof(PJ_descriptor_import_provider_v1_t), 0, &MosaicoToolbox::descriptorQueryThunk,
      &MosaicoToolbox::descriptorStartThunk};
};

}  // namespace mosaico

#ifdef PJ_TOOLBOX_PLUGIN_NAMED
PJ_TOOLBOX_PLUGIN_NAMED(mosaico::MosaicoToolbox, MosaicoToolbox, kMosaicoPanelManifest)
PJ_DIALOG_PLUGIN_NAMED(mosaico::MosaicoDialog, MosaicoDialog, kMosaicoPanelManifest)
#else
PJ_TOOLBOX_PLUGIN(mosaico::MosaicoToolbox, kMosaicoPanelManifest)
PJ_DIALOG_PLUGIN(mosaico::MosaicoDialog, kMosaicoPanelManifest)
#endif
