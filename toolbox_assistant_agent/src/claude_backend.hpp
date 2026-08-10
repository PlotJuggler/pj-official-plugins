// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "llm_backend.hpp"
#include "mcp_http_server.hpp"
#include "stream_json.hpp"  // TurnMetrics, reported by the CLI in its result record

namespace assistant_agent {

// Remote backend driving the user's Claude Code CLI subscription headlessly —
// NO Anthropic API key, no per-token billing. Per turn it spawns
// `claude -p --output-format stream-json ... "<message>"` and parses the
// streamed events for the transcript. Because headless Claude accepts external
// tools ONLY via MCP, the assistant's tools are exposed through an in-plugin
// localhost MCP server (started lazily on the first turn); Claude calls it over
// HTTP and those calls run through the same GuiExecutor as every other backend.
// Conversation context is preserved across turns via the CLI's --resume with the
// session id Claude reports.
class ClaudeBackend : public LlmBackend {
 public:
  ClaudeBackend(std::string cli_path, std::string model);
  ~ClaudeBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // Probe: `<cli> --version` exits 0.
  [[nodiscard]] BackendTestResult testConnection() const override;

  // Cost and token split of the turn that just finished, as the CLI reported
  // it. `valid` is false if the turn never produced a result record.
  [[nodiscard]] const TurnMetrics& lastTurnMetrics() const {
    return last_metrics_;
  }
  // Set when the CLI reported a usage window that is no longer "allowed" —
  // a batch driver should stop rather than keep burning failed turns.
  [[nodiscard]] bool rateLimited() const {
    return rate_limited_;
  }

 private:
  // Bring up the MCP server on first use, bound to this turn's tool surface.
  bool ensureMcpServer(const TurnTools& tools, std::string& error);

  std::string cli_path_;
  std::string model_;
  std::string session_id_;  // Claude session for --resume continuity
  // The catalog listing already sent into this conversation. --resume carries
  // the whole history forward, so re-sending an identical listing every turn
  // would be pure waste; re-sending a CHANGED one is how the model finds out
  // the user loaded something else.
  std::string sent_catalog_;
  TurnMetrics last_metrics_;
  bool rate_limited_ = false;
  std::unique_ptr<McpHttpServer> mcp_;
  // 0600 temp file holding the MCP config (bearer token inside); created with
  // the server, removed in the destructor.
  std::string mcp_config_path_;
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
