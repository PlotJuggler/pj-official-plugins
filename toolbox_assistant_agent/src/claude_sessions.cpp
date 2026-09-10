// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "claude_sessions.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/time_format.hpp>
#include <string_view>

namespace assistant_agent {

namespace {

using nlohmann::json;

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

}  // namespace

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
  if (const std::optional<std::string> cfg = PJ::sdk::getEnv("CLAUDE_CONFIG_DIR")) {
    base = *cfg;
  } else if (const std::optional<std::string> home = PJ::sdk::getEnv("HOME")) {
    base = *home + "/.claude";
  } else {
    return {};
  }
  return std::filesystem::path(base) / "projects" / claudeCwdSlug(work_dir);
}

std::optional<std::time_t> parseIso8601Utc(const std::string& iso8601) {
  // PJ::parseIso8601Utc (pj_base/time_format.hpp) owns the parse: it is strict
  // about the calendar, understands the fractional seconds and the zone suffix
  // (Z or a numeric offset), and is locale-independent. It answers in Unix
  // nanoseconds; the drawer only ever labels to the minute, so this narrows to
  // whole seconds, flooring rather than truncating so a pre-epoch timestamp
  // lands on the second that contains it.
  const std::optional<std::int64_t> ns = PJ::parseIso8601Utc(iso8601);
  if (!ns) {
    return std::nullopt;
  }
  constexpr std::int64_t kNsPerSecond = 1'000'000'000;
  std::int64_t seconds = *ns / kNsPerSecond;
  if (*ns % kNsPerSecond != 0 && *ns < 0) {
    --seconds;
  }
  return static_cast<std::time_t>(seconds);
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
