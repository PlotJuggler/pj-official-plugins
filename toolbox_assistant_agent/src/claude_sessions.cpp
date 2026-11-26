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
#include <pj_base/time_format.hpp>
#include <string_view>

#include "platform_util.hpp"  // getEnvUtf8, userHomeDir, utf8ToPath

namespace assistant_agent {

namespace {

using nlohmann::json;

// The first "text" block in a message's `content`: a plain string (a real user
// prompt) or an array of typed blocks. nullopt when there is no text, e.g. a user
// message carrying only tool_result blocks.
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
  // Claude Code maps every non-alphanumeric character to '-'
  // ("/home/u/Work/plotjuggler_sdk" -> "-home-u-Work-plotjuggler-sdk"). `cwd` is
  // UTF-8, so a multi-byte code point (lead byte plus 0x80-0xBF continuation bytes)
  // collapses to exactly ONE '-'.
  std::string slug;
  slug.reserve(cwd.size());
  std::size_t i = 0;
  while (i < cwd.size()) {
    const unsigned char c = static_cast<unsigned char>(cwd[i]);
    if (c < 0x80) {
      const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      slug += alnum ? static_cast<char>(c) : '-';
      ++i;
      continue;
    }
    slug += '-';
    ++i;
    while (i < cwd.size() && (static_cast<unsigned char>(cwd[i]) & 0xC0) == 0x80) {
      ++i;
    }
  }
  return slug;
}

std::filesystem::path claudeSessionsDir(const std::string& work_dir) {
  std::filesystem::path base;
  if (const std::optional<std::string> cfg = getEnvUtf8("CLAUDE_CONFIG_DIR")) {
    base = utf8ToPath(*cfg);
  } else if (const std::optional<std::string> home = userHomeDir()) {
    base = utf8ToPath(*home) / ".claude";
  } else {
    return {};
  }
  return base / "projects" / claudeCwdSlug(work_dir);
}

std::optional<std::time_t> parseIso8601Utc(const std::string& iso8601) {
  // Narrows PJ::parseIso8601Utc's Unix nanoseconds to whole seconds, flooring (not
  // truncating) so a pre-epoch timestamp lands on the second that contains it.
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
  // Not strftime's %b: the label must not change shape with the locale.
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
        continue;
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
        summary.last_ts = t;
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
    }

    if (summary.assistant_messages == 0) {
      continue;  // a cancelled turn or test run, not a real conversation
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
    return a.last_ts > b.last_ts;  // ISO 8601 sorts lexicographically
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
      continue;
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
      continue;
    }

    // assistant: one row per text/tool_use block, left unmerged (appendAssistant folds them).
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
  // remove(path, ec) leaves ec clear for an already-absent file.
  return !ec;
}

}  // namespace assistant_agent
