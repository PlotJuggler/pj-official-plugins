// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "claude_sessions.hpp"  // ConversationSummary, for listConversations()
#include "harness_memory.hpp"   // HarnessMemory + composePayload
#include "llm_backend.hpp"
#include "mcp_loopback.hpp"
#include "turn_metrics.hpp"

namespace assistant_agent {

// The env var Codex reads the MCP bearer token from (`-c
// mcp_servers.pj.bearer_token_env_var=...`). The token itself never appears
// on the command line — it travels only through the child's environment (see
// subprocess.hpp's extra_env), set right before exec. A fixed name because
// this plugin ever configures exactly one MCP server ("pj").
inline constexpr const char* kCodexMcpTokenEnvVar = "PJ_ASSISTANT_MCP_TOKEN";

// The command line handed to `codex`, built where a test can read it — same
// reason as buildClaudeArgv (claude_backend.hpp): the flags that keep
// headless Codex off the user's machine are a fixed list, and nothing should
// be free to quietly shorten it. Verified live on Codex CLI 0.153.0
// (docs/ARCHITECTURE.md, "The Codex backend"); the flags are measured, not a
// preference — do not "improve" them.
//
// `workdir` is passed as `-C <workdir>` on a fresh conversation; a resume
// (`session_id` non-empty) omits `-C` entirely and runs `codex exec resume
// <session_id>` instead — Codex resolves a resumed thread from its own
// session store regardless of cwd. `-c` values are TOML: a string value's
// argv entry carries the double quotes INSIDE it (e.g. the literal argv
// string is `web_search="disabled"`); a boolean is bare. The prompt is never
// an argv entry — it goes on stdin, appended as `-` (the caller writes it via
// runProcess's `stdin_data`).
[[nodiscard]] std::vector<std::string> buildCodexArgv(
    const std::string& cli_path, const std::string& workdir, const std::string& mcp_url,
    const std::string& token_env_name, const std::string& instructions_path, const std::string& model,
    const std::string& session_id);

// Remote backend driving the user's Codex CLI subscription headlessly, the
// Codex counterpart to ClaudeBackend. Per turn it spawns `codex exec --json
// ...` and parses the streamed events (codex_stream.hpp) for the transcript.
// Shares the loopback MCP server (mcp_loopback.hpp) and the stable work dir
// (harness_workdir.hpp) with ClaudeBackend, but reaches the server through a
// `-c mcp_servers.pj.*` config plus an env-var bearer token rather than a
// `--mcp-config` file — Codex has no such flag.
//
// Three deliberate deviations from Claude, all measured (docs/ARCHITECTURE.md):
//  - Code Mode (Codex's JS tool-call host) cannot be disabled without also
//    disabling our own MCP tools, so it stays on; the resulting startup
//    notice item is not an error (it arrives before turn.started).
//  - `approval_policy="never"` still requires
//    `mcp_servers.pj.default_tools_approval_mode="approve"` or every tool
//    call fails outright.
//  - Resuming a thread passes no `-C`: the process cwd is irrelevant.
class CodexBackend : public LlmBackend {
 public:
  // `memory` must outlive the backend; see ClaudeBackend's own comment.
  CodexBackend(std::string cli_path, std::string model, std::shared_ptr<HarnessMemory> memory = nullptr);
  ~CodexBackend() override;

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // Probe: `<cli> --version` exits 0.
  [[nodiscard]] BackendTestResult testConnection() const override;

  // Reads the CLI's own models_cache.json (codex_models.hpp) -- unlike
  // Claude, Codex maintains an actual on-disk catalog.
  [[nodiscard]] std::vector<ModelChoice> availableModels() const override {
    return listModels();
  }
  // Static twin of availableModels(), so the settings code (assistant_dialog.cpp's
  // BackendSpec::models) can list this backend's models without constructing one.
  [[nodiscard]] static std::vector<ModelChoice> listModels();

  // The harness's own session store for this CLI's cwd (codex_sessions.hpp).
  [[nodiscard]] std::vector<ConversationSummary> listConversations() override;
  [[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::string& id) override;
  bool deleteConversation(const std::string& id) override;

  // Token split of the turn that just finished (Codex reports no cost, only
  // tokens — TurnMetrics::cost_usd/api_ms stay 0). `valid` is false if the
  // turn never produced a turn.completed record.
  [[nodiscard]] const TurnMetrics& lastTurnMetrics() const {
    return last_metrics_;
  }

 private:
  bool ensureWorkDir(std::string& error);
  // Lazily writes kSystemPrompt, plus the no-sub-agents addendum, to a
  // private mkstemp 0600 file, memoized for the life of this backend and
  // unlinked in the destructor. `model_instructions_file` is Codex's
  // replacement for its own "You are Codex" persona, the same job
  // --append-system-prompt does for Claude.
  bool ensureInstructionsFile(std::string& error);

  std::string cli_path_;
  std::string model_;
  std::shared_ptr<HarnessMemory> memory_;  // never null after construction
  TurnMetrics last_metrics_;
  McpLoopback mcp_;
  std::string work_dir_;
  std::string instructions_path_;
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
