// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary, for listConversations()
#include "harness_memory.hpp"   // HarnessMemory + composePayload
#include "llm_backend.hpp"
#include "stream_json.hpp"  // TurnMetrics, reported by the CLI in its result record

namespace assistant_agent {

// The command line handed to the CLI, built where a test can read it.
//
// This exists as a free function for one reason: the flags that keep headless
// Claude off the user's machine are three entries in a vector, and nothing
// stopped a well-meant change from removing them. `ClaudeBackendCommandLineIsLocked`
// asserts them; that property is not negotiable.
//
// An empty `model` or `session_id` simply omits its flag.
[[nodiscard]] std::vector<std::string> buildClaudeArgv(
    const std::string& cli_path, const std::string& mcp_config_path, const std::string& system_prompt,
    const std::string& model, const std::string& session_id);

// composePayload now lives in harness_memory.hpp (backend-agnostic, shared
// with CodexBackend) and is pulled in transitively via the #include above.

// Remote backend driving the user's Claude Code CLI subscription headlessly —
// NO Anthropic API key, no per-token billing. Per turn it spawns
// `claude -p --output-format stream-json ... "<message>"` and parses the
// streamed events for the transcript. Headless Claude accepts external tools
// ONLY via MCP, so it is pointed at this plugin's endpoint on PlotJuggler's own
// MCP server (TurnTools::mcp, wrapped in a --mcp-config file); PlotJuggler runs
// the tools. Conversation context is preserved across turns via the CLI's
// --resume with the session id Claude reports — held in a caller-owned
// HarnessMemory so that rebuilding this object (a settings change) does not
// silently start over.
class ClaudeBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; the dialog owns one per conversation and
  // lends the same one to every backend it builds. A null pointer is treated as
  // a fresh private memory, so tests can construct without one.
  ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory = nullptr);
  // Unlinks the --mcp-config file, if one was written.
  ~ClaudeBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // The instructions passed as --append-system-prompt; assistantInstructions()
  // (system prompt + product policy) unless replaced. Only the benchmark
  // runner replaces them, to measure without the policy.
  void setInstructions(std::string instructions) {
    instructions_ = std::move(instructions);
  }

  // Probe: `<cli> --version` exits 0.
  [[nodiscard]] BackendTestResult testConnection() const override;

  // A curated list, not a catalog: headless Claude Code writes no model list
  // to disk and has no "list models" command (`claude --help` names only the
  // aliases). See claude_backend.cpp for the ordering rationale.
  [[nodiscard]] std::vector<ModelChoice> availableModels() const override {
    return listModels();
  }
  // Static twin of availableModels(), so the settings code (assistant_dialog.cpp's
  // BackendSpec::models) can list this backend's models without constructing one.
  [[nodiscard]] static std::vector<ModelChoice> listModels();

  // The harness's own session store for this CLI's cwd (claude_sessions.hpp).
  // Resolves the work dir on first use exactly like sendUserMessage does; an
  // empty list/transcript/false is the honest answer when that fails (no
  // XDG_STATE_HOME/HOME), not an error the caller has to handle separately.
  [[nodiscard]] std::vector<ConversationSummary> listConversations() override;
  [[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::string& id) override;
  bool deleteConversation(const std::string& id) override;

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
  // Resolve (once) the private directory the CLI runs in (harness_workdir.hpp).
  // The CLI reads its cwd's CLAUDE.md and project state as context, so
  // inheriting the host app's cwd would inject whatever project PlotJuggler
  // happened to be launched from into the panel's system prompt. Pairs with
  // --restricted, which covers the user-level side (settings, global
  // CLAUDE.md). The directory is STABLE (under XDG state), not a fresh temp
  // dir: the CLI indexes its sessions by cwd, and resuming a persisted
  // conversation (--resume after a plugin restart) only works when every
  // instance runs in the same place.
  bool ensureWorkDir(std::string& error);
  // The --mcp-config file for `endpoint` (one "pj" http server, the token in
  // an Authorization header): a private 0600 temp file, since argv is
  // world-readable via /proc/<pid>/cmdline. Memoized while the endpoint is
  // unchanged; a new endpoint's file replaces (and unlinks) the old one. ""
  // when it could not be written.
  std::string mcpConfigFile(const PJ::sdk::McpEndpoint& endpoint);

  std::string cli_path_;
  std::string model_;
  std::string instructions_;
  std::shared_ptr<HarnessMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  bool rate_limited_ = false;
  // 0700 stable directory the CLI runs in (see ensureWorkDir). Never removed:
  // it holds the CLI's session state, which is what --resume comes back to.
  std::string work_dir_;
  std::string mcp_config_path_;
  PJ::sdk::McpEndpoint mcp_config_endpoint_;  // what mcp_config_path_ holds
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
