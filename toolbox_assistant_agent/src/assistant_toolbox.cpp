// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// AssistantAgent — an LLM assistant delivered as a PJ4 Toolbox plugin. It
// drives the user's own agent CLI and hands it a private endpoint on
// PlotJuggler's MCP server; PlotJuggler serves the tools there and runs them
// with this plugin as owner, so the model can reach only what those tools
// allow (read data, create derived series/objects and markers, compose tabs
// and scene views of its own) and nothing on the machine outside them.

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
    // Read path for the catalog digest every turn carries (catalog_digest.hpp):
    // the catalog itself, and playback for an object topic's display-time range.
    dialog_.setCatalogHosts(
        toolboxHost(), services.get<PJ::sdk::PlaybackHostService>().value_or(PJ::sdk::PlaybackHostView{}));
    // The tools: this plugin's private endpoint on PlotJuggler's MCP server
    // (pj.mcp_endpoint.v1). PlotJuggler runs every tool the model calls, with
    // this plugin as their owner. Optional at bind, so a compatible host that
    // omits the service still loads the plugin; every turn then fails with a
    // message that says why.
    dialog_.setMcpEndpointHost(
        services.get<PJ::sdk::McpEndpointHostService>().value_or(PJ::sdk::McpEndpointHostView{}));
    // What the host offers (pj.feature_catalog.v1): built-in features, the
    // plugins loaded, installed or installable. Optional — a host that omits it
    // leaves the view unbound and every turn carries the reference built into
    // this plugin instead (fallback_features.hpp).
    feature_catalog_view_ = services.get<PJ::sdk::FeatureCatalogService>().value_or(PJ::sdk::FeatureCatalogView{});
    dialog_.setFeatureCatalogProvider([this]() { return feature_catalog_view_; });
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
  PJ::sdk::FeatureCatalogView feature_catalog_view_;
};

}  // namespace assistant_agent

PJ_TOOLBOX_PLUGIN(assistant_agent::AssistantToolbox, kAssistantPanelManifest)
PJ_DIALOG_PLUGIN(assistant_agent::AssistantDialog)
