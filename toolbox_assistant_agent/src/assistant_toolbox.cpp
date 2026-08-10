// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// AssistantAgent — an LLM assistant delivered as a PJ4 Toolbox plugin. Behind
// the plugin ABI the model is "just a user of the SDK": it can only reach the
// services the host wires into bind() (read catalog/series, create derived
// timeseries + markers, persist settings). No delete/destroy service exists in
// the ABI, so the assistant is non-destructive by construction, not by policy.

#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>

#include "assistant_dialog.hpp"
#include "assistant_panel_manifest.hpp"

namespace assistant_agent {

class AssistantToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    // Read path (catalog + series) and notifyDataChanged. Captured lazily so the
    // tool layer obtains a fresh view per call (on the GUI thread, in onTick).
    dialog_.setHostProvider([this]() { return toolboxHost(); });
    dialog_.setRuntimeHostProvider([this]() { return runtimeHost(); });
    // Write path: pj.data_processors.v1 backs create_derived_series (transform)
    // and create_markers. Optional — if the host omits it, those tools return a
    // clean "service unavailable" to the model instead of failing to bind.
    dp_view_ = services.get<PJ::sdk::DataProcessorsHostService>().value_or(PJ::sdk::DataProcessorsHostView{});
    dialog_.setDataProcessorsProvider([this]() { return dp_view_; });
    // Optional pj.settings.v1 store (QSettings-like persistence). An unbound
    // view reads defaults / drops writes, so this is safe when the host omits it.
    dialog_.setSettings(services.get<PJ::sdk::SettingsStoreService>().value_or(PJ::sdk::SettingsView{}));
    return PJ::okStatus();
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  AssistantDialog dialog_;
  PJ::sdk::DataProcessorsHostView dp_view_;
};

}  // namespace assistant_agent

PJ_TOOLBOX_PLUGIN(assistant_agent::AssistantToolbox, kAssistantPanelManifest)
PJ_DIALOG_PLUGIN(assistant_agent::AssistantDialog)
