// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace assistant_agent {

// Text helpers shared by the session-store readers (claude_sessions.hpp,
// codex_sessions.hpp): tool-name prefix strip, catalog-prefix strip (with the notes
// composePayload can prepend, harness_memory.hpp), and title truncation.

// Notes composePayload (harness_memory.hpp) prepends alongside a re-sent catalog or
// features listing. stripCatalogPrefix recognizes them by their OPENING words only, so
// the rest of a sentence can be reworded without orphaning notes already written into
// session files; the static_asserts below pin those openings.
inline constexpr std::string_view kCatalogChangedNote =
    "(The loaded data changed; the listing above replaces the earlier one.)";
inline constexpr std::string_view kResumedConversationNote =
    "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the "
    "ones that do. The listing above is the data loaded now.)";
inline constexpr std::string_view kFeaturesChangedNote = "(The PlotJuggler features listing changed.)";

// Header line of every features block, host's (featuresHostHeader) or built-in
// reference's (kFallbackFeaturesHeader). stripCatalogPrefix matches the whole first
// line, so user text merely starting with "PlotJuggler features" is not taken for one.
inline constexpr std::string_view kFeaturesHostHeaderUnversioned = "PlotJuggler features (from the host):";
inline constexpr std::string_view kFeaturesHeaderVersionOpening = "PlotJuggler features (PlotJuggler ";
inline constexpr std::string_view kFeaturesHostHeaderClosing = ", from the host):";
inline constexpr std::string_view kFeaturesFallbackHeaderClosing = ", built into this assistant):";
inline constexpr std::string_view kFallbackFeaturesHeader =
    "PlotJuggler features (PlotJuggler 4.1, built into this assistant):";

// "PlotJuggler features (PlotJuggler <version>, from the host):", unversioned when the
// host reports no version.
[[nodiscard]] inline std::string featuresHostHeader(std::string_view host_version) {
  if (host_version.empty()) {
    return std::string(kFeaturesHostHeaderUnversioned);
  }
  std::string out(kFeaturesHeaderVersionOpening);
  out += host_version;
  out += kFeaturesHostHeaderClosing;
  return out;
}

// Whether `line` is a header featuresHostHeader or kFallbackFeaturesHeader produces.
// Any version is accepted so sessions from another build or host still strip.
[[nodiscard]] constexpr bool isFeaturesHeader(std::string_view line) {
  if (line == kFeaturesHostHeaderUnversioned) {
    return true;
  }
  if (!line.starts_with(kFeaturesHeaderVersionOpening)) {
    return false;
  }
  const std::string_view rest = line.substr(kFeaturesHeaderVersionOpening.size());
  for (const std::string_view closing : {kFeaturesHostHeaderClosing, kFeaturesFallbackHeaderClosing}) {
    if (rest.size() > closing.size() && rest.ends_with(closing)) {  // a non-empty version
      return true;
    }
  }
  return false;
}
static_assert(isFeaturesHeader(kFallbackFeaturesHeader));
static_assert(!isFeaturesHeader("PlotJuggler features I need:"));

namespace detail {

constexpr std::string_view kCatalogChangedNoteOpening = "(The loaded data changed";
constexpr std::string_view kResumedConversationNoteOpening = "(Resumed conversation";
constexpr std::string_view kFeaturesChangedNoteOpening = "(The PlotJuggler features listing changed";
static_assert(kCatalogChangedNote.substr(0, kCatalogChangedNoteOpening.size()) == kCatalogChangedNoteOpening);
static_assert(
    kResumedConversationNote.substr(0, kResumedConversationNoteOpening.size()) == kResumedConversationNoteOpening);
static_assert(kFeaturesChangedNote.substr(0, kFeaturesChangedNoteOpening.size()) == kFeaturesChangedNoteOpening);

// Index of the first character after the blank line (run of '\n') ending the block at
// `from`; npos when there is none. composePayload always shapes its output as
// `catalog\n\n[note\n\n]text`.
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

// "mcp__pj__read_series" -> "read_series": "pj" is the server name this plugin gives
// PlotJuggler's MCP endpoint (ClaudeBackend::mcpConfigFile, buildCodexArgv). Live and
// replayed tool rows both go through this.
inline std::string prettyToolName(const std::string& name) {
  const std::string prefix = "mcp__pj__";
  return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
}

// Title fallback: the first prompt, collapsed to one line and capped so a drawer row
// never wraps. No ellipsis.
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
    // Back off to a code-point boundary: cutting a multi-byte UTF-8 sequence would
    // yield an invalid string.
    std::size_t cut = kMaxTitleBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    text.resize(cut);
  }
  return text;
}

// Undoes composePayload's prepends (harness_memory.hpp) on a user message loaded from
// disk: `[features\n\n[note\n\n]][catalog\n\n[note\n\n]]text`. A features block is
// recognized by its whole header line (isFeaturesHeader), a catalog by "Loaded data";
// text starting with neither is returned unchanged. A block with no blank line is
// malformed or truncated and left as is.
inline std::string stripCatalogPrefix(const std::string& text) {
  std::string_view rest = text;
  const auto starts = [&](std::string_view prefix) { return rest.substr(0, prefix.size()) == prefix; };
  // Drops the block `rest` starts with and, if what follows opens with one of
  // `notes`, that note too. False when the block has no blank line.
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
  if (isFeaturesHeader(rest.substr(0, rest.find('\n'))) && !skipBlock({detail::kFeaturesChangedNoteOpening})) {
    return text;
  }
  if (starts("Loaded data")) {
    skipBlock({detail::kCatalogChangedNoteOpening, detail::kResumedConversationNoteOpening});
  }
  return std::string(rest);
}

}  // namespace assistant_agent
