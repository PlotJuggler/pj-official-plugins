// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "claude_sessions.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace assistant_agent {

namespace {

using nlohmann::json;

// The opening words stripCatalogPrefix matches — pinned against the full
// notes so a rewording that touches the opening fails to compile instead of
// silently orphaning every note already written into a session file.
constexpr std::string_view kCatalogChangedNoteOpening = "(The loaded data changed";
constexpr std::string_view kResumedConversationNoteOpening = "(Resumed conversation";
static_assert(kCatalogChangedNote.substr(0, kCatalogChangedNoteOpening.size()) == kCatalogChangedNoteOpening);
static_assert(
    kResumedConversationNote.substr(0, kResumedConversationNoteOpening.size()) == kResumedConversationNoteOpening);

// The first "text" block in a `user`/`assistant` message's `content`, which is
// either a plain string (typical for a real user prompt) or an array of typed
// blocks (a tool_result-bearing user message, or any assistant message).
// Returns nullopt when there is no text to show — e.g. a user message that
// carries only tool_result blocks, which loadTranscript intentionally ignores.
std::optional<std::string> firstTextBlock(const json& content) {
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (!content.is_array()) {
    return std::nullopt;
  }
  for (const json& block : content) {
    if (block.is_object() && block.value("type", std::string{}) == "text") {
      return block.value("text", std::string{});
    }
  }
  return std::nullopt;
}

// Skip past a block that ends with a blank line (a run of consecutive '\n'),
// landing on the first character of whatever follows `from`. `std::npos` when
// no blank line exists past `from`. See stripCatalogPrefix: composePayload
// (claude_backend.cpp) always shapes its output as `catalog\n\n[note\n\n]text`,
// so this is the one primitive both strips need.
std::size_t skipPastBlankLine(const std::string& s, std::size_t from) {
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

// Title fallback: the first prompt, catalog stripped, collapsed to one line
// and capped so a drawer row never wraps. No ellipsis — this is a list label,
// not a place to signal truncation.
std::string truncateTitle(std::string text) {
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

}  // namespace

std::string prettyToolName(const std::string& name) {
  const std::string prefix = "mcp__pj__";
  return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
}

std::string claudeCwdSlug(const std::string& cwd) {
  std::string slug = cwd;
  for (char& c : slug) {
    if (c == '/' || c == '.') {
      c = '-';
    }
  }
  return slug;
}

std::filesystem::path claudeSessionsDir(const std::string& work_dir) {
  std::string base;
  if (const char* cfg = std::getenv("CLAUDE_CONFIG_DIR"); cfg != nullptr && cfg[0] != '\0') {
    base = cfg;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    base = std::string(home) + "/.claude";
  } else {
    return {};
  }
  return std::filesystem::path(base) / "projects" / claudeCwdSlug(work_dir);
}

std::string stripCatalogPrefix(const std::string& text) {
  if (text.rfind("Loaded data", 0) != 0) {
    return text;
  }
  const std::size_t after_catalog = skipPastBlankLine(text, 0);
  if (after_catalog == std::string::npos) {
    return text;  // truncated/malformed catalog block -- leave it, don't guess
  }
  std::string rest = text.substr(after_catalog);
  if (rest.rfind(kCatalogChangedNoteOpening, 0) == 0 || rest.rfind(kResumedConversationNoteOpening, 0) == 0) {
    const std::size_t after_note = skipPastBlankLine(rest, 0);
    rest = after_note == std::string::npos ? std::string{} : rest.substr(after_note);
  }
  return rest;
}

std::optional<std::time_t> parseIso8601Utc(const std::string& iso8601) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  // sscanf rather than strptime: the latter does not exist on MSVC, and this
  // plugin is built for Windows too. The fractional seconds and the 'Z' that
  // follow are irrelevant to a minute-resolution label.
  if (std::sscanf(iso8601.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return std::nullopt;
  }
  // Civil date -> days since the Unix epoch (Howard Hinnant's algorithm), so
  // no timegm()/_mkgmtime() portability split is needed.
  const int shifted_year = year - (month <= 2 ? 1 : 0);
  const int era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(shifted_year - era * 400);
  const unsigned shifted_month = static_cast<unsigned>(month > 2 ? month - 3 : month + 9);
  const unsigned day_of_year = (153U * shifted_month + 2U) / 5U + static_cast<unsigned>(day) - 1U;
  const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  const long long days = static_cast<long long>(era) * 146097LL + static_cast<long long>(day_of_era) - 719468LL;
  return static_cast<std::time_t>(days * 86400LL + hour * 3600LL + minute * 60LL + second);
}

std::string formatShortDate(const std::string& iso8601, bool local) {
  const std::optional<std::time_t> when = parseIso8601Utc(iso8601);
  if (!when.has_value()) {
    return {};
  }
  std::tm tm{};
#if defined(_WIN32)
  if ((local ? localtime_s(&tm, &*when) : gmtime_s(&tm, &*when)) != 0) {
    return {};
  }
#else
  if ((local ? localtime_r(&*when, &tm) : gmtime_r(&*when, &tm)) == nullptr) {
    return {};
  }
#endif
  // Fixed English month names rather than strftime's %b: the drawer's date is
  // a compact key next to the title, and it must not change shape with the
  // user's locale.
  static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (tm.tm_mon < 0 || tm.tm_mon > 11) {
    return {};
  }
  char buf[32];
  const int n =
      std::snprintf(buf, sizeof(buf), "%d %s %02d:%02d", tm.tm_mday, kMonths[tm.tm_mon], tm.tm_hour, tm.tm_min);
  if (n <= 0) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

std::vector<ConversationSummary> listConversations(const std::filesystem::path& dir) {
  std::vector<ConversationSummary> out;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return out;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }
    std::ifstream file(entry.path());
    if (!file) {
      continue;
    }

    ConversationSummary summary;
    summary.id = entry.path().stem().string();
    std::string ai_title;
    std::optional<std::string> first_user_text;

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty()) {
        continue;
      }
      const json rec = json::parse(line, nullptr, /*allow_exceptions=*/false);
      if (!rec.is_object()) {
        continue;  // corrupt line: skip it, keep reading the rest of the file
      }
      const auto type_it = rec.find("type");
      if (type_it == rec.end() || !type_it->is_string()) {
        continue;
      }
      const std::string& type = type_it->get_ref<const std::string&>();

      if (const auto ts = rec.find("timestamp"); ts != rec.end() && ts->is_string()) {
        const std::string& t = ts->get_ref<const std::string&>();
        if (summary.first_ts.empty()) {
          summary.first_ts = t;
        }
        summary.last_ts = t;  // records are appended in order; last write wins
      }

      if (type == "assistant") {
        ++summary.assistant_messages;
      } else if (type == "ai-title") {
        if (const auto at = rec.find("aiTitle");
            at != rec.end() && at->is_string() && !at->get_ref<const std::string&>().empty()) {
          ai_title = at->get_ref<const std::string&>();
        }
      } else if (type == "user" && !first_user_text.has_value()) {
        const auto msg = rec.find("message");
        if (msg != rec.end() && msg->is_object()) {
          if (const auto content = msg->find("content"); content != msg->end()) {
            first_user_text = firstTextBlock(*content);
          }
        }
      }
      // attachment / queue-operation / last-prompt / atis-latch / anything
      // future: not part of this listing, ignored by construction (no branch).
    }

    if (summary.assistant_messages == 0) {
      continue;  // a cancelled turn or a test run against this cwd, not a real conversation
    }
    if (!ai_title.empty()) {
      summary.title = ai_title;
    } else if (first_user_text.has_value()) {
      summary.title = truncateTitle(stripCatalogPrefix(*first_user_text));
    }
    if (summary.title.empty()) {
      summary.title = "Untitled";
    }
    out.push_back(std::move(summary));
  }

  std::sort(out.begin(), out.end(), [](const ConversationSummary& a, const ConversationSummary& b) {
    return a.last_ts > b.last_ts;  // ISO 8601 sorts lexicographically -- newest first
  });
  return out;
}

std::vector<ChatMessage> loadTranscript(const std::filesystem::path& dir, const std::string& id) {
  std::vector<ChatMessage> out;
  std::ifstream file(dir / (id + ".jsonl"));
  if (!file) {
    return out;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    const json rec = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!rec.is_object()) {
      continue;
    }
    const auto type_it = rec.find("type");
    if (type_it == rec.end() || !type_it->is_string()) {
      continue;
    }
    const std::string& type = type_it->get_ref<const std::string&>();
    if (type != "user" && type != "assistant") {
      continue;  // ai-title, attachment, queue-operation, last-prompt, ... : not transcript rows
    }
    const auto msg = rec.find("message");
    if (msg == rec.end() || !msg->is_object()) {
      continue;
    }
    const auto content_it = msg->find("content");
    if (content_it == msg->end()) {
      continue;
    }
    const json& content = *content_it;

    if (type == "user") {
      if (const std::optional<std::string> text = firstTextBlock(content); text.has_value()) {
        out.push_back({ChatMessage::Role::User, stripCatalogPrefix(*text)});
      }
      continue;  // tool_result blocks (if any) carry no row of their own
    }

    // assistant: one row per text/tool_use block, in order, left unmerged --
    // the caller replays these through ChatSession's own appendAssistant, the
    // same call the live path uses to fold a streamed reply into one row.
    if (!content.is_array()) {
      continue;
    }
    for (const json& block : content) {
      if (!block.is_object()) {
        continue;
      }
      const std::string btype = block.value("type", std::string{});
      if (btype == "text") {
        const std::string text = block.value("text", std::string{});
        if (!text.empty()) {
          out.push_back({ChatMessage::Role::Assistant, text});
        }
      } else if (btype == "tool_use") {
        out.push_back({ChatMessage::Role::Tool, prettyToolName(block.value("name", std::string{}))});
      }
    }
  }
  return out;
}

bool deleteConversation(const std::filesystem::path& dir, const std::string& id) {
  std::error_code ec;
  std::filesystem::remove(dir / (id + ".jsonl"), ec);
  // remove(path, ec) does not treat "already absent" as an error: ec stays
  // clear either way, which is exactly "ignore absence".
  return !ec;
}

}  // namespace assistant_agent
