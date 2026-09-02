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

namespace assistant_agent {

// Read-only, tolerant access to the Claude Code harness's own session store —
// `~/.claude/projects/<slug>/<id>.jsonl`, one append-only file per
// conversation. This plugin never writes there (only the CLI does); it reads
// what already exists so the panel's conversation list/transcript/title come
// from the same place `--resume` resumes from, instead of a second, easily
// stale copy of our own. See docs/ARCHITECTURE.md, "Where the conversation
// lives".
//
// Every function here is Qt-free and defensive: a missing directory, a
// corrupt line, or a record type this build has never heard of degrade to
// "skip it", never to a thrown exception — this parser runs on the same
// ClaudeBackend calls the rest of the plugin treats as exception barriers.

// `${CLAUDE_CONFIG_DIR:-$HOME/.claude}/projects/<slug>`, where `slug` is
// `work_dir` with every '/' and '.' turned into '-' (the CLI's own scheme —
// verified against a real store: "/home/alvvm/.local/state/pj-assistant-cli"
// -> "-home-alvvm--local-state-pj-assistant-cli"). Returns an empty path when
// neither variable resolves a base directory; callers treat that as "list
// nothing" rather than guessing.
[[nodiscard]] std::filesystem::path claudeSessionsDir(const std::string& work_dir);

// The slug half of the above, split out so it is testable without touching
// HOME/CLAUDE_CONFIG_DIR.
[[nodiscard]] std::string claudeCwdSlug(const std::string& cwd);

// One row for the conversations drawer. `title` already resolves the
// ai-title/first-prompt/"Untitled" fallback chain; the drawer only adds the
// short date behind it (list rows are matched by TEXT, and two conversations
// can otherwise share a title).
struct ConversationSummary {
  std::string id;  // the jsonl file's stem == the CLI's session id
  std::string title;
  std::string first_ts;  // ISO 8601, the first record with a timestamp
  std::string last_ts;   // ISO 8601, the last record with a timestamp
  int assistant_messages = 0;
};

// Every `*.jsonl` in `dir`, newest-first by `last_ts`, excluding sessions with
// no `assistant` record at all (a cancelled turn, or a smoke test run against
// this cwd — never something the user had a reply from). A missing or
// unreadable directory yields an empty list, not an error.
[[nodiscard]] std::vector<ConversationSummary> listConversations(const std::filesystem::path& dir);

// The rows of one conversation, in file order, ready to replay through the
// SAME ChatSession calls the live path uses (addUser/appendAssistant/addTool
// — see assistant_dialog.cpp's switchToConversation): `user` text becomes a
// User row with the catalog prefix stripped; `assistant` text blocks become
// Assistant rows (left unmerged here — appendAssistant does the merging, the
// same way it folds a streamed reply's chunks into one row); `assistant`
// tool_use blocks become Tool rows, formatted exactly like the live
// BackendEvent::ToolActivity text (tool name only, "mcp__pj__" prefix
// stripped — the live path does not carry the arguments either). `tool_result`
// blocks are ignored (their content already surfaces as the model's next
// reply). An unreadable file, or one that does not exist, yields an empty
// vector.
[[nodiscard]] std::vector<ChatMessage> loadTranscript(const std::filesystem::path& dir, const std::string& id);

// Removes `<id>.jsonl`. A missing file is not an error — deleting an already-
// gone conversation (a double click, a race with Claude's own retention) is a
// no-op success, not a failure the caller has to special-case.
[[nodiscard]] bool deleteConversation(const std::filesystem::path& dir, const std::string& id);

// "mcp__pj__read_series" -> "read_series": the MCP namespace this plugin
// chose for its tools (claude_backend.cpp's allowedToolsArg), stripped for a
// transcript line — the live ToolActivity row and a replayed one go through
// this one function.
[[nodiscard]] std::string prettyToolName(const std::string& name);

// The two notes composePayload (claude_backend.cpp) can prepend alongside a
// (re-)sent catalog listing. stripCatalogPrefix recognizes them by their
// OPENING words only, so the rest of each sentence may be reworded without
// orphaning the notes already written into session files; the static_asserts
// in claude_sessions.cpp pin those openings.
inline constexpr std::string_view kCatalogChangedNote =
    "(The loaded data changed; the listing above replaces the earlier one.)";
inline constexpr std::string_view kResumedConversationNote =
    "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the "
    "ones that do. The listing above is the data loaded now.)";

// Undo composePayload's catalog prepend (claude_backend.cpp) on a user
// message loaded back from disk: `catalog + "\n" + [note] + "\n" + text`. Text
// not starting with "Loaded data" (every turn after the first one that saw a
// given catalog) is returned unchanged.
[[nodiscard]] std::string stripCatalogPrefix(const std::string& text);

// "2026-09-01T15:17:30.326Z" (UTC, as the harness writes it) -> seconds since
// the Unix epoch. nullopt on anything it cannot parse. Pure, so the drawer's
// date label is testable without a clock or a time zone.
[[nodiscard]] std::optional<std::time_t> parseIso8601Utc(const std::string& iso8601);

// The drawer's per-row date suffix: "1 Sep 15:17". `local` renders in the
// user's time zone (the drawer's case — a conversation held at 09:15 must not
// read 07:15); false keeps UTC, for deterministic tests. Fixed English month
// names, so the label's shape does not depend on the locale. Returns "" on a
// timestamp it cannot parse (the caller then just shows the title alone).
[[nodiscard]] std::string formatShortDate(const std::string& iso8601, bool local = true);

}  // namespace assistant_agent
