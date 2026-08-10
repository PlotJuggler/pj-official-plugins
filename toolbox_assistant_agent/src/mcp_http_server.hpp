// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "tool_registry.hpp"

namespace ix {
class HttpServer;
}

namespace assistant_agent {

// A minimal MCP server over localhost HTTP, used to expose the assistant's
// ToolRegistry to the headless Claude Code CLI (which accepts external tools
// ONLY via MCP). It binds 127.0.0.1 on a scanned port, guards every request
// with a random per-session bearer token, and answers the three JSON-RPC
// methods Claude needs — initialize, tools/list, tools/call — with immediate
// application/json responses (no SSE). tools/call runs through the same
// ToolInvoker (GuiExecutor) as every other backend, so tools still execute on
// the GUI thread.
class McpHttpServer {
 public:
  McpHttpServer(const ToolRegistry& registry, ToolInvoker invoker);
  ~McpHttpServer();

  McpHttpServer(const McpHttpServer&) = delete;
  McpHttpServer& operator=(const McpHttpServer&) = delete;

  // Bind a loopback port and start serving. Returns false if no port was free.
  bool start();
  void stop();

  [[nodiscard]] int port() const {
    return port_;
  }
  [[nodiscard]] const std::string& token() const {
    return token_;
  }
  [[nodiscard]] std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/mcp";
  }

  // Build the inline `--mcp-config` JSON Claude expects: one http server named
  // "pj" with the bearer token in an Authorization header.
  [[nodiscard]] std::string mcpConfigJson() const;

 private:
  // Dispatch one JSON-RPC request object; returns the response object, or a null
  // json for a notification (no id -> no reply).
  nlohmann::json handleRpc(const nlohmann::json& req);
  // The actual dispatch; handleRpc wraps it in the server-thread exception
  // barrier (untrusted client JSON + throwing typed getters).
  nlohmann::json handleRpcChecked(const nlohmann::json& req);

  const ToolRegistry& registry_;
  ToolInvoker invoker_;
  std::unique_ptr<ix::HttpServer> server_;
  int port_ = 0;
  std::string token_;
};

}  // namespace assistant_agent
