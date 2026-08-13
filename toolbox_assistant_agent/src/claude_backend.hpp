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

// What has to survive this backend object for the conversation to continue.
//
// It is deliberately NOT owned by ClaudeBackend: the backend is a transport
// (a CLI path and a model name) and gets rebuilt whenever either changes, while
// the conversation belongs to the user and does not. Kept together in one
// struct because both fields answer the same question — what this conversation
// has already been told.
struct ClaudeMemory {
  std::string session_id;  // Claude session for --resume continuity
  // The catalog listing already sent into this conversation. --resume carries
  // the whole history forward, so re-sending an identical listing every turn
  // would be pure waste; re-sending a CHANGED one is how the model finds out
  // the user loaded something else.
  std::string sent_catalog;
};

// The command line handed to the CLI, built where a test can read it.
//
// This exists as a free function for one reason: the flags that keep headless
// Claude off the user's machine are three entries in a vector, and nothing
// stopped a well-meant change from removing them. `ClaudeBackendCommandLineIsLocked`
// asserts them. See the "not negotiable" note in ROADMAP.md.
//
// `allowed_tools` is the comma-separated `mcp__pj__*` list; an empty `model` or
// `session_id` simply omits its flag.
[[nodiscard]] std::vector<std::string> buildClaudeArgv(
    const std::string& cli_path, const std::string& mcp_config_path, const std::string& allowed_tools,
    const std::string& system_prompt, const std::string& model, const std::string& session_id);

// The `mcp__pj__<name>` list the CLI is allowed to call, one entry per registered
// tool. Every entry carries that prefix by construction, which is what keeps a
// built-in tool from being whitelisted by accident.
[[nodiscard]] std::string allowedToolsArg(const ToolRegistry& registry);

// Remote backend driving the user's Claude Code CLI subscription headlessly —
// NO Anthropic API key, no per-token billing. Per turn it spawns
// `claude -p --output-format stream-json ... "<message>"` and parses the
// streamed events for the transcript. Because headless Claude accepts external
// tools ONLY via MCP, the assistant's tools are exposed through an in-plugin
// localhost MCP server (started lazily on the first turn); Claude calls it over
// HTTP and those calls run through the same GuiExecutor as every other backend.
// Conversation context is preserved across turns via the CLI's --resume with the
// session id Claude reports — held in a caller-owned ClaudeMemory so that
// rebuilding this object (a settings change) does not silently start over.
class ClaudeBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; the dialog owns one per conversation and
  // lends the same one to every backend it builds. A null pointer is treated as
  // a fresh private memory, so tests can construct without one.
  ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<ClaudeMemory> memory = nullptr);
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
  std::shared_ptr<ClaudeMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  bool rate_limited_ = false;
  std::unique_ptr<McpHttpServer> mcp_;
  // 0600 temp file holding the MCP config (bearer token inside); created with
  // the server, removed in the destructor.
  std::string mcp_config_path_;
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
