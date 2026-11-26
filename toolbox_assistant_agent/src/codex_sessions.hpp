// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "chat_session.hpp"
#include "claude_sessions.hpp"  // ConversationSummary

namespace assistant_agent {

// Read-only, tolerant access to the Codex CLI's own session store,
// `${CODEX_HOME:-~/.codex}/sessions/YYYY/MM/DD/rollout-<timestamp>-<uuid>.jsonl`, one
// append-only file per conversation. Same contract as claude_sessions.hpp (never
// written to, defensive against corrupt lines and unknown record shapes), under
// distinct names, and the listing takes a `work_dir` because the cwd filter has to
// happen inside it (Claude's store is already one directory per cwd).
//
// `codex exec` prepends harness-injected "user"-role messages ("<environment_context>...",
// "<recommended_plugins>...", the global AGENTS.md as "# AGENTS.md instructions...")
// before the real prompt, with nothing in the JSON marking them synthetic. Both
// functions below recognize them by their opening text (isHarnessInjectedUserText in
// codex_sessions.cpp) and skip them for the title and the transcript.

// `${CODEX_HOME:-$HOME/.codex}`, the single place this is resolved (codexSessionsDir()
// and codexModelsCacheFile() append to it). Empty when neither variable resolves.
[[nodiscard]] std::filesystem::path codexHomeDir();

// `codexHomeDir()/sessions`. Empty means "list nothing".
[[nodiscard]] std::filesystem::path codexSessionsDir();

// Every `rollout-*.jsonl` under `sessions_dir` (recursing the YYYY/MM/DD
// layout) whose line 1 is a `session_meta` record with `payload.cwd ==
// work_dir`, newest-first by last_ts, excluding sessions with zero assistant
// messages. A missing or unreadable directory yields an empty list.
[[nodiscard]] std::vector<ConversationSummary> listCodexConversations(
    const std::filesystem::path& sessions_dir, const std::string& work_dir);

// The rows of one conversation (the file whose name ends with `-<id>.jsonl`), in file
// order, for replay through the live path's ChatSession calls. No tool rows: session
// files carry no verified `McpToolCall` event_msg item to replay.
[[nodiscard]] std::vector<ChatMessage> loadCodexTranscript(
    const std::filesystem::path& sessions_dir, const std::string& id);

// Removes the `rollout-*-<id>.jsonl` file. A missing file is a no-op success.
[[nodiscard]] bool deleteCodexConversation(const std::filesystem::path& sessions_dir, const std::string& id);

}  // namespace assistant_agent
