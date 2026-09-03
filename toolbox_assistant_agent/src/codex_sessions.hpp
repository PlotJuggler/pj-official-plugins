// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "chat_session.hpp"
#include "claude_sessions.hpp"  // ConversationSummary -- one shape, shared by every harness's drawer row

namespace assistant_agent {

// Read-only, tolerant access to the Codex CLI's own session store —
// `${CODEX_HOME:-~/.codex}/sessions/YYYY/MM/DD/rollout-<timestamp>-<uuid>.jsonl`,
// one append-only file per conversation. Mirrors claude_sessions.hpp's
// contract exactly (never written to, defensive against corrupt lines and
// record shapes this build has never heard of) but under distinct function
// names — `listConversations`/`loadTranscript`/`deleteConversation` already
// name Claude's own free functions in this namespace, and Codex's `dir`
// parameter takes an extra `work_dir` (the cwd filter has to happen inside
// the listing here; Claude's own store is already split one directory per
// cwd, so it needs no such filter).
//
// `codex exec` prepends a couple of harness-injected "user"-role messages
// ("<environment_context>...", "<recommended_plugins>...") before the actual
// prompt on (at least) a session's first turn — verified against real files
// under ~/.codex/sessions/2026/09/03/. Nothing in the JSON marks them as
// synthetic, so both functions below recognize them by their opening tag
// (codex_sessions.cpp's isHarnessInjectedUserText) and treat them exactly
// like a Claude `developer` record: skipped for both the title and the
// transcript.

// `${CODEX_HOME:-~/.codex}/sessions`. Empty when neither variable resolves a
// base directory; callers treat that as "list nothing".
[[nodiscard]] std::filesystem::path codexSessionsDir();

// Every `rollout-*.jsonl` under `sessions_dir` (recursing the YYYY/MM/DD
// layout) whose line 1 is a `session_meta` record with `payload.cwd ==
// work_dir`, newest-first by last_ts, excluding sessions with zero assistant
// messages. A missing or unreadable directory yields an empty list.
[[nodiscard]] std::vector<ConversationSummary> listCodexConversations(
    const std::filesystem::path& sessions_dir, const std::string& work_dir);

// The rows of one conversation (found by the file whose name ends with
// `-<id>.jsonl`), in file order, ready to replay through the same ChatSession
// calls the live path uses. No tool rows: the real session files under
// ~/.codex/sessions/2026/09/03/ never carry an `McpToolCall` event_msg item,
// so there is nothing verified to replay — see codex_backend_brief.md.
[[nodiscard]] std::vector<ChatMessage> loadCodexTranscript(
    const std::filesystem::path& sessions_dir, const std::string& id);

// Removes the `rollout-*-<id>.jsonl` file. A missing file is a no-op success.
[[nodiscard]] bool deleteCodexConversation(const std::filesystem::path& sessions_dir, const std::string& id);

}  // namespace assistant_agent
