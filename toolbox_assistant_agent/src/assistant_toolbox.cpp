// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// An LLM assistant toolbox. It drives the user's own agent CLI and hands it a private endpoint
// on PlotJuggler's MCP server; PlotJuggler runs the tools there with this plugin as owner, so
// the model reaches only what those tools allow (read data, create derived series/objects and
// markers, tabs and scene views of its own) and nothing else on the machine.

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
    // Catalog digest sources (catalog_digest.hpp); playback gives an object topic's display-time range.
    dialog_.setCatalogHosts(
        toolboxHost(), services.get<PJ::sdk::PlaybackHostService>().value_or(PJ::sdk::PlaybackHostView{}));
    // This plugin's endpoint on the host's MCP server (pj.mcp_endpoint.v1). Optional at bind: a
    // host that omits it still loads the plugin, and every turn then fails saying why.
    dialog_.setMcpEndpointHost(
        services.get<PJ::sdk::McpEndpointHostService>().value_or(PJ::sdk::McpEndpointHostView{}));
    // Optional pj.feature_catalog.v1; without it every turn carries the built-in reference
    // (fallback_features.hpp).
    feature_catalog_view_ = services.get<PJ::sdk::FeatureCatalogService>().value_or(PJ::sdk::FeatureCatalogView{});
    dialog_.setFeatureCatalogProvider([this]() { return feature_catalog_view_; });
    // Optional pj.settings.v1; an unbound view reads defaults and drops writes.
    dialog_.setSettings(services.get<PJ::sdk::SettingsStoreService>().value_or(PJ::sdk::SettingsView{}));
    return PJ::okStatus();
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  AssistantDialog dialog_;
  PJ::sdk::FeatureCatalogView feature_catalog_view_;
};

}  // namespace assistant_agent

PJ_TOOLBOX_PLUGIN(assistant_agent::AssistantToolbox, kAssistantPanelManifest)
PJ_DIALOG_PLUGIN(assistant_agent::AssistantDialog)
