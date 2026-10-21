// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <initializer_list>
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
// (re-)sent catalog listing, and the one it can prepend alongside a re-sent
// features listing. stripCatalogPrefix recognizes them by their OPENING words
// only, so the rest of each sentence may be reworded without orphaning the
// notes already written into session files; the static_asserts below pin
// those openings.
inline constexpr std::string_view kCatalogChangedNote =
    "(The loaded data changed; the listing above replaces the earlier one.)";
inline constexpr std::string_view kResumedConversationNote =
    "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the "
    "ones that do. The listing above is the data loaded now.)";
inline constexpr std::string_view kFeaturesChangedNote = "(The PlotJuggler features listing changed.)";

// First words of every features block (features_digest.cpp and
// fallback_features.hpp build their header line from this constant), which
// is how stripCatalogPrefix tells a features block from user text.
inline constexpr std::string_view kFeaturesListingOpening = "PlotJuggler features";

namespace detail {

constexpr std::string_view kCatalogChangedNoteOpening = "(The loaded data changed";
constexpr std::string_view kResumedConversationNoteOpening = "(Resumed conversation";
constexpr std::string_view kFeaturesChangedNoteOpening = "(The PlotJuggler features listing changed";
static_assert(kCatalogChangedNote.substr(0, kCatalogChangedNoteOpening.size()) == kCatalogChangedNoteOpening);
static_assert(
    kResumedConversationNote.substr(0, kResumedConversationNoteOpening.size()) == kResumedConversationNoteOpening);
static_assert(kFeaturesChangedNote.substr(0, kFeaturesChangedNoteOpening.size()) == kFeaturesChangedNoteOpening);

// Skip past a block that ends with a blank line (a run of consecutive '\n'),
// landing on the first character of whatever follows `from`. `std::npos` when
// no blank line exists past `from`. See stripCatalogPrefix: composePayload
// (harness_memory.hpp) always shapes its output as `catalog\n\n[note\n\n]text`,
// so this is the one primitive both strips need.
inline std::size_t skipPastBlankLine(std::string_view s, std::size_t from) {
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

// Undo composePayload's prepends (harness_memory.hpp) on a user message
// loaded back from disk: `[features\n\n[note\n\n]][catalog\n\n[note\n\n]]text`
// (sessions written before the features block existed carry
// `catalog\n[note]\ntext`, which strips the same way). A leading features
// block is recognized by kFeaturesListingOpening, a catalog by "Loaded data";
// text starting with neither (every turn after the first one that saw a given
// listing) is returned unchanged.
inline std::string stripCatalogPrefix(const std::string& text) {
  std::string_view rest = text;
  const auto starts = [&](std::string_view prefix) { return rest.substr(0, prefix.size()) == prefix; };
  // Drops the block `rest` starts with (through its blank line) and, when the
  // text after it opens with one of `notes`, that note too. False when the
  // block has no blank line: truncated/malformed, leave it, don't guess.
  const auto skipBlock = [&](std::initializer_list<std::string_view> notes) {
    const std::size_t after = detail::skipPastBlankLine(rest, 0);
    if (after == std::string::npos) {
      return false;
    }
    rest.remove_prefix(after);
    for (const std::string_view note : notes) {
      if (starts(note)) {
        const std::size_t after_note = detail::skipPastBlankLine(rest, 0);
        rest = after_note == std::string::npos ? std::string_view{} : rest.substr(after_note);
        break;
      }
    }
    return true;
  };
  if (starts(kFeaturesListingOpening) && !skipBlock({detail::kFeaturesChangedNoteOpening})) {
    return text;
  }
  if (starts("Loaded data")) {
    skipBlock({detail::kCatalogChangedNoteOpening, detail::kResumedConversationNoteOpening});
  }
  return std::string(rest);
}

}  // namespace assistant_agent
