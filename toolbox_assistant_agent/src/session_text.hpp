// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace assistant_agent {

// Small text helpers shared by every harness's session-store reader
// (claude_sessions.hpp, codex_sessions.hpp): the tool-name prefix strip, the
// catalog-prefix strip (and the two notes composePayload can prepend
// alongside it, harness_memory.hpp), and the drawer's title truncation.
// Header-only and Qt-free, like stream_json.hpp/codex_stream.hpp, so either
// session-store reader can pull it in without a second copy of these rules.

// The two notes composePayload (harness_memory.hpp) can prepend alongside a
// (re-)sent catalog listing. stripCatalogPrefix recognizes them by their
// OPENING words only, so the rest of each sentence may be reworded without
// orphaning the notes already written into session files; the static_asserts
// below pin those openings.
inline constexpr std::string_view kCatalogChangedNote =
    "(The loaded data changed; the listing above replaces the earlier one.)";
inline constexpr std::string_view kResumedConversationNote =
    "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the "
    "ones that do. The listing above is the data loaded now.)";

namespace detail {

constexpr std::string_view kCatalogChangedNoteOpening = "(The loaded data changed";
constexpr std::string_view kResumedConversationNoteOpening = "(Resumed conversation";
static_assert(kCatalogChangedNote.substr(0, kCatalogChangedNoteOpening.size()) == kCatalogChangedNoteOpening);
static_assert(
    kResumedConversationNote.substr(0, kResumedConversationNoteOpening.size()) == kResumedConversationNoteOpening);

// Skip past a block that ends with a blank line (a run of consecutive '\n'),
// landing on the first character of whatever follows `from`. `std::npos` when
// no blank line exists past `from`. See stripCatalogPrefix: composePayload
// (harness_memory.hpp) always shapes its output as `catalog\n\n[note\n\n]text`,
// so this is the one primitive both strips need.
inline std::size_t skipPastBlankLine(const std::string& s, std::size_t from) {
  const std::size_t nl = s.find("\n\n", from);
  if (nl == std::string::npos) {
    return std::string::npos;
  }
  std::size_t pos = nl + 1;
  while (pos < s.size() && s[pos] == '\n') {
    ++pos;
  }
  return pos;
}

}  // namespace detail

// "mcp__pj__read_series" -> "read_series": the MCP namespace this plugin
// chose for its tools (claude_backend.cpp's allowedToolsArg), stripped for a
// transcript line — the live ToolActivity row and a replayed one go through
// this one function.
inline std::string prettyToolName(const std::string& name) {
  const std::string prefix = "mcp__pj__";
  return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
}

// Title fallback: the first prompt, catalog stripped, collapsed to one line
// and capped so a drawer row never wraps. No ellipsis — this is a list label,
// not a place to signal truncation.
inline std::string truncateTitle(std::string text) {
  for (char& c : text) {
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
  }
  std::size_t begin = text.find_first_not_of(' ');
  std::size_t end = text.find_last_not_of(' ');
  text = begin == std::string::npos ? std::string{} : text.substr(begin, end - begin + 1);
  constexpr std::size_t kMaxTitleBytes = 48;
  if (text.size() > kMaxTitleBytes) {
    // Back off to a code-point boundary: a cut through a multi-byte UTF-8
    // sequence ("Identificación…") would hand the list an invalid string.
    std::size_t cut = kMaxTitleBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    text.resize(cut);
  }
  return text;
}

// Undo composePayload's catalog prepend (harness_memory.hpp) on a user
// message loaded back from disk: `catalog + "\n" + [note] + "\n" + text`. Text
// not starting with "Loaded data" (every turn after the first one that saw a
// given catalog) is returned unchanged.
inline std::string stripCatalogPrefix(const std::string& text) {
  if (text.rfind("Loaded data", 0) != 0) {
    return text;
  }
  const std::size_t after_catalog = detail::skipPastBlankLine(text, 0);
  if (after_catalog == std::string::npos) {
    return text;  // truncated/malformed catalog block -- leave it, don't guess
  }
  std::string rest = text.substr(after_catalog);
  if (rest.rfind(detail::kCatalogChangedNoteOpening, 0) == 0 ||
      rest.rfind(detail::kResumedConversationNoteOpening, 0) == 0) {
    const std::size_t after_note = detail::skipPastBlankLine(rest, 0);
    rest = after_note == std::string::npos ? std::string{} : rest.substr(after_note);
  }
  return rest;
}

}  // namespace assistant_agent
