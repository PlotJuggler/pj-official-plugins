// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chat_session.hpp"
#include "session_text.hpp"  // prettyToolName, stripCatalogPrefix, truncateTitle

namespace assistant_agent {

// Read-only, tolerant access to the Claude Code harness's own session store,
// `~/.claude/projects/<slug>/<id>.jsonl`, one append-only file per conversation.
// Only the CLI writes there; reading it keeps the conversation list, transcript and
// title identical to what `--resume` resumes from (docs/ARCHITECTURE.md, "Where the
// conversation lives").
//
// Qt-free and defensive: a missing directory, corrupt line or unknown record type
// is skipped, never thrown.

// `${CLAUDE_CONFIG_DIR:-$HOME/.claude}/projects/<slug>`, where `slug` is `work_dir`
// with every non-alphanumeric ASCII character turned into '-' and every multi-byte
// UTF-8 code point collapsed to one '-' (the CLI's own scheme:
// "/home/user/.local/state/pj-assistant-cli" -> "-home-user--local-state-pj-assistant-cli").
// Empty when neither variable resolves a base directory; callers list nothing.
[[nodiscard]] std::filesystem::path claudeSessionsDir(const std::string& work_dir);

// The slug half of the above, testable without touching HOME/CLAUDE_CONFIG_DIR.
[[nodiscard]] std::string claudeCwdSlug(const std::string& cwd);

// One row for the conversations drawer. `title` already resolves the
// ai-title/first-prompt/"Untitled" fallback chain; the drawer adds a short date behind
// it because list rows are matched by TEXT and two conversations can share a title.
struct ConversationSummary {
  std::string id;  // the jsonl file's stem == the CLI's session id
  std::string title;
  std::string first_ts;  // ISO 8601, the first record with a timestamp
  std::string last_ts;   // ISO 8601, the last record with a timestamp
  int assistant_messages = 0;
};

// Every `*.jsonl` in `dir`, newest-first by `last_ts`, excluding sessions with no
// `assistant` record (a cancelled turn or smoke test the user never got a reply from).
// A missing or unreadable directory yields an empty list.
[[nodiscard]] std::vector<ConversationSummary> listConversations(const std::filesystem::path& dir);

// The rows of one conversation in file order, for replay through the same ChatSession
// calls as the live path (addUser/appendAssistant/addTool; see switchToConversation).
// `user` text becomes a User row with the catalog prefix stripped; `assistant` text
// blocks become Assistant rows, left unmerged because appendAssistant does the folding;
// `tool_use` blocks become Tool rows formatted like the live BackendEvent::ToolActivity
// text (tool name only, "mcp__pj__" prefix stripped). `tool_result` blocks are ignored.
// An unreadable or missing file yields an empty vector.
[[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::filesystem::path& dir, const std::string& id);

// Removes `<id>.jsonl`. A missing file is a no-op success (double click, race with the
// CLI's own retention).
[[nodiscard]] bool deleteConversation(const std::filesystem::path& dir, const std::string& id);

// "2026-09-01T15:17:30.326Z" (UTC, as the harness writes it) -> seconds since the Unix
// epoch; nullopt when unparsable. Pure, so the date label is testable without a clock.
[[nodiscard]] std::optional<std::time_t> parseIso8601Utc(const std::string& iso8601);

// The drawer's per-row date suffix: "1 Sep 15:17". `local` renders in the user's time
// zone; false keeps UTC for deterministic tests. Fixed English month names so the
// label does not change shape with the locale. "" when the timestamp is unparsable.
[[nodiscard]] std::string formatShortDate(const std::string& iso8601, bool local = true);

}  // namespace assistant_agent
