// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

namespace assistant_agent {

// The outcome of a tool call, handed back to the model verbatim. `content` is
// the text (usually compact JSON) the model reads; `ok=false` marks a failure
// the model should see and react to (e.g. a rejected transform), NOT a plugin
// crash — errors are data here, never exceptions across the ABI.
struct ToolResult {
  bool ok = true;
  std::string content;

  static ToolResult success(std::string content) {
    return {true, std::move(content)};
  }
  static ToolResult failure(std::string content) {
    return {false, std::move(content)};
  }
};

// Host handles the executors reach through. Every executor runs on the GUI
// (host-callback) thread — the GuiExecutor guarantees that — so these views may
// be used directly. Obtained fresh per turn from the dialog's providers.
struct ToolContext {
  PJ::sdk::ToolboxHostView host;              // catalog snapshot + readSeries
  PJ::sdk::DataProcessorsHostView dp;         // createTransform / createMarkers / validateScript
  std::function<void()> notify_data_changed;  // runtimeHost().notifyDataChanged() after a create
  std::string language = "luau";              // transform/marker script backend
};

}  // namespace assistant_agent
