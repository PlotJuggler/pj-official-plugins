// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary
#include "harness_memory.hpp"   // HarnessMemory
#include "llm_backend.hpp"
#include "turn_metrics.hpp"

namespace assistant_agent {

// The env var Codex reads the MCP bearer token from (`-c mcp_servers.pj.bearer_token_env_var=...`).
// The token travels only through the child's environment (subprocess.hpp's extra_env),
// never argv. Fixed name: this plugin configures exactly one MCP server ("pj").
inline constexpr const char* kCodexMcpTokenEnvVar = "PJ_ASSISTANT_MCP_TOKEN";

// The command line handed to `codex`, a free function so a test can read it (see
// buildClaudeArgv): the flags that keep headless Codex off the user's machine are a fixed
// list. They were measured against the real CLI (docs/ARCHITECTURE.md, "The Codex
// backend"); do not "improve" them.
//
// `workdir` is passed as `-C <workdir>` on a fresh conversation; a resume (`session_id`
// non-empty) omits `-C` and runs `codex exec resume <session_id>`, since Codex resolves a
// resumed thread from its own session store regardless of cwd. `-c` values are TOML: a
// string's argv entry carries the double quotes INSIDE it (`web_search="disabled"`), a
// boolean is bare. The prompt is never an argv entry: it goes on stdin, via the trailing `-`.
[[nodiscard]] std::vector<std::string> buildCodexArgv(
    const std::string& cli_path, const std::string& workdir, const std::string& mcp_url,
    const std::string& token_env_name, const std::string& instructions_path, const std::string& model,
    const std::string& session_id);

// Drives the user's Codex CLI subscription headlessly, the counterpart to ClaudeBackend.
// Per turn it spawns `codex exec --json` and parses the events (codex_stream.hpp). Uses
// the same MCP endpoint (TurnTools::mcp) and work dir (harness_workdir.hpp), but reaches
// the server through `-c mcp_servers.pj.*` plus an env-var bearer token, since Codex has
// no `--mcp-config` flag.
//
// Deviations from Claude, all measured (docs/ARCHITECTURE.md):
//  - Code Mode (Codex's JS tool-call host) cannot be disabled without also disabling our
//    MCP tools, so it stays on; its startup notice item is not an error (it arrives
//    before turn.started).
//  - `approval_policy="never"` still requires
//    `mcp_servers.pj.default_tools_approval_mode="approve"` or every tool call fails.
//  - Resuming a thread passes no `-C`: the process cwd is irrelevant.
class CodexBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; see ClaudeBackend.
  CodexBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory = nullptr);
  ~CodexBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // Written to model_instructions_file (plus the no-sub-agents addendum);
  // assistantInstructions() unless replaced (only the benchmark runner does). Call before
  // the first turn: the file is written once.
  void setInstructions(std::string instructions) {
    instructions_ = std::move(instructions);
  }

  [[nodiscard]] BackendTestResult testConnection() const override;

  // Reads the CLI's own models_cache.json (codex_models.hpp).
  [[nodiscard]] std::vector<ModelChoice> availableModels() const override {
    return listModels();
  }
  // Static twin of availableModels() for BackendSpec::models, which has no instance.
  [[nodiscard]] static std::vector<ModelChoice> listModels();

  // The harness's own session store for this CLI's cwd (codex_sessions.hpp).
  [[nodiscard]] std::vector<ConversationSummary> listConversations() override;
  [[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::string& id) override;
  bool deleteConversation(const std::string& id) override;

  // Token split of the last turn (Codex reports no cost: cost_usd/api_ms stay 0); `valid`
  // is false if the turn produced no turn.completed record.
  [[nodiscard]] const TurnMetrics& lastTurnMetrics() const {
    return last_metrics_;
  }

 private:
  bool ensureWorkDir(std::string& error);
  // Lazily writes instructions_ plus the no-sub-agents addendum to a private 0600 file,
  // memoized for the backend's life and unlinked in the destructor.
  // `model_instructions_file` replaces Codex's own "You are Codex" persona, like
  // --append-system-prompt for Claude.
  bool ensureInstructionsFile(std::string& error);

  std::string cli_path_;
  std::string model_;
  std::string instructions_;
  std::shared_ptr<HarnessMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  std::string work_dir_;
  std::string instructions_path_;
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
