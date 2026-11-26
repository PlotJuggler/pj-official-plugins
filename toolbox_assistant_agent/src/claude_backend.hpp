// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary
#include "harness_memory.hpp"   // HarnessMemory
#include "llm_backend.hpp"
#include "stream_json.hpp"  // TurnMetrics

namespace assistant_agent {

// The command line handed to the CLI, a free function so a test can read it: the flags
// that keep headless Claude off the user's machine are three vector entries, and
// `ClaudeBackendCommandLineIsLocked` asserts them. An empty `model` or `session_id`
// omits its flag.
[[nodiscard]] std::vector<std::string> buildClaudeArgv(
    const std::string& cli_path, const std::string& mcp_config_path, const std::string& system_prompt,
    const std::string& model, const std::string& session_id);

// Drives the user's Claude Code CLI subscription headlessly (no API key, no per-token
// billing). Per turn it spawns `claude -p --output-format stream-json` and parses the
// streamed events. Headless Claude accepts external tools ONLY via MCP, so it is pointed
// at this plugin's endpoint on PlotJuggler's MCP server (TurnTools::mcp, in a
// --mcp-config file) and PlotJuggler runs the tools. Context carries across turns via
// --resume with the session id Claude reports, held in a caller-owned HarnessMemory so
// rebuilding this object (a settings change) does not restart the conversation.
class ClaudeBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; the dialog lends the same one to every backend it
  // builds. Null means a fresh private memory (tests).
  ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory = nullptr);
  // Unlinks the --mcp-config file, if one was written.
  ~ClaudeBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // Passed as --append-system-prompt; assistantInstructions() unless replaced (only the
  // benchmark runner does, to measure without the policy).
  void setInstructions(std::string instructions) {
    instructions_ = std::move(instructions);
  }

  [[nodiscard]] BackendTestResult testConnection() const override;

  // A curated list: headless Claude Code has no on-disk model list or "list models" command.
  [[nodiscard]] std::vector<ModelChoice> availableModels() const override {
    return listModels();
  }
  // Static twin of availableModels() for BackendSpec::models, which has no instance.
  [[nodiscard]] static std::vector<ModelChoice> listModels();

  // The harness's own session store for this CLI's cwd (claude_sessions.hpp). Resolves
  // the work dir like sendUserMessage; empty/false when that fails.
  [[nodiscard]] std::vector<ConversationSummary> listConversations() override;
  [[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::string& id) override;
  bool deleteConversation(const std::string& id) override;

  // Cost and token split of the last turn as the CLI reported it; `valid` is false if
  // the turn produced no result record.
  [[nodiscard]] const TurnMetrics& lastTurnMetrics() const {
    return last_metrics_;
  }
  // Set when the CLI reported a usage window that is no longer "allowed"; a batch driver
  // should stop.
  [[nodiscard]] bool rateLimited() const {
    return rate_limited_;
  }

 private:
  // Resolves (once) the private directory the CLI runs in (harness_workdir.hpp). The CLI
  // reads its cwd's CLAUDE.md and project state, so inheriting the host's cwd would inject
  // whatever project PlotJuggler was launched from; --restricted covers the user-level
  // side. STABLE (under XDG state), not a fresh temp dir: the CLI indexes sessions by cwd,
  // so --resume after a restart only works from the same place.
  bool ensureWorkDir(std::string& error);
  // The --mcp-config file for `endpoint` (one "pj" http server, token in an Authorization
  // header): a private 0600 temp file, since argv is readable via /proc/<pid>/cmdline.
  // Memoized while the endpoint is unchanged; a new endpoint's file replaces the old one.
  // "" when it could not be written.
  std::string mcpConfigFile(const PJ::sdk::McpEndpoint& endpoint);

  std::string cli_path_;
  std::string model_;
  std::string instructions_;
  std::shared_ptr<HarnessMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  bool rate_limited_ = false;
  // Never removed: it holds the session state --resume comes back to.
  std::string work_dir_;
  std::string mcp_config_path_;
  PJ::sdk::McpEndpoint mcp_config_endpoint_;  // what mcp_config_path_ holds
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
