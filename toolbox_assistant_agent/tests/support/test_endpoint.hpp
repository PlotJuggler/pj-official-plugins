// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <string>
#include <utility>

#include "llm_backend.hpp"  // TurnTools

namespace assistant_agent::testing {

// The endpoint a backend test runs its turns against. Nothing listens there,
// which a fake CLI never notices: it never dials it.
inline const PJ::sdk::McpEndpoint kTestEndpoint{"http://127.0.0.1:8765/mcp", "test-bearer-token"};

// A turn's TurnTools with `endpoint` and no listings.
inline TurnTools toolsFor(const PJ::sdk::McpEndpoint& endpoint = kTestEndpoint) {
  TurnTools tools;
  tools.mcp = endpoint;
  return tools;
}

// A running PlotJuggler's MCP endpoint, for the opt-in live tests:
// ASSISTANT_MCP_URL and ASSISTANT_MCP_TOKEN. Empty when either is unset.
//
// These are the PUBLIC endpoint of the instance (the url and token in its
// <AppLocalData>/mcp-instances/<pid>.json), NOT the private endpoint the panel
// acquires through pj.mcp_endpoint.v1: that one exists only for a plugin loaded
// inside PlotJuggler, and a standalone test binary has no way to ask for it. The
// public token reaches more tools than the private profile does (open_file,
// unload_file, export_series, ...), so the live tests show that the CLI wiring
// works (the config file or env var carrying the token, the allow-list, a real
// tool round trip), not what the plugin's own endpoint serves; the private
// profile is pinned by PlotJuggler's own McpEndpointTest (pj_app/tests/
// mcp_server_test.cpp). Point them at a scratch PlotJuggler instance: a model
// driving the public tools can load and unload files and export data.
inline std::optional<PJ::sdk::McpEndpoint> liveEndpoint() {
  std::optional<std::string> url = PJ::sdk::getEnv("ASSISTANT_MCP_URL");
  std::optional<std::string> token = PJ::sdk::getEnv("ASSISTANT_MCP_TOKEN");
  if (!url || !token) {
    return std::nullopt;
  }
  return PJ::sdk::McpEndpoint{std::move(*url), std::move(*token)};
}

inline constexpr const char* kNoLiveEndpoint =
    "set ASSISTANT_MCP_URL and ASSISTANT_MCP_TOKEN to a running PlotJuggler's MCP endpoint (start it with --mcp; "
    "<AppLocalData>/mcp-instances/<pid>.json holds the port and token)";

}  // namespace assistant_agent::testing
