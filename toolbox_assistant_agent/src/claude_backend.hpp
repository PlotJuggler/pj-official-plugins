// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary, for listConversations()
#include "harness_memory.hpp"   // HarnessMemory + composePayload
#include "llm_backend.hpp"
#include "mcp_loopback.hpp"
#include "stream_json.hpp"  // TurnMetrics, reported by the CLI in its result record

namespace assistant_agent {

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

// composePayload now lives in harness_memory.hpp (backend-agnostic, shared
// with CodexBackend) and is pulled in transitively via the #include above.

// Remote backend driving the user's Claude Code CLI subscription headlessly —
// NO Anthropic API key, no per-token billing. Per turn it spawns
// `claude -p --output-format stream-json ... "<message>"` and parses the
// streamed events for the transcript. Because headless Claude accepts external
// tools ONLY via MCP, the assistant's tools are exposed through an in-plugin
// localhost MCP server (started lazily on the first turn); Claude calls it over
// HTTP and those calls run through the same GuiExecutor as every other backend.
// Conversation context is preserved across turns via the CLI's --resume with the
// session id Claude reports — held in a caller-owned HarnessMemory so that
// rebuilding this object (a settings change) does not silently start over.
class ClaudeBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; the dialog owns one per conversation and
  // lends the same one to every backend it builds. A null pointer is treated as
  // a fresh private memory, so tests can construct without one.
  ClaudeBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory = nullptr);
  ~ClaudeBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

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

  std::string cli_path_;
  std::string model_;
  std::shared_ptr<HarnessMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  bool rate_limited_ = false;
  // The loopback MCP server plus (Claude-only) the --mcp-config file wrapping
  // its token (mcp_loopback.hpp).
  McpLoopback mcp_;
  // 0700 stable directory the CLI runs in (see ensureWorkDir). Never removed:
  // it holds the CLI's session state, which is what --resume comes back to.
  std::string work_dir_;
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
