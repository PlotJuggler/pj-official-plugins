// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "codex_sessions.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace assistant_agent {

namespace {

using nlohmann::json;

// See codex_sessions.hpp's file comment: codex exec prepends these two
// harness-injected "user"-role messages before the real prompt, and nothing
// in the JSON marks them as synthetic — the opening tag is the only signal.
bool isHarnessInjectedUserText(const std::string& text) {
  return text.rfind("<environment_context>", 0) == 0 || text.rfind("<recommended_plugins>", 0) == 0;
}

// The first block carrying a "text" field in a response_item message's
// `content` array (Codex always shapes it as an array of typed blocks --
// {"type":"input_text"/"output_text","text":...} -- never a bare string).
std::optional<std::string> firstTextBlock(const json& content) {
  if (!content.is_array()) {
    return std::nullopt;
  }
  for (const json& block : content) {
    if (block.is_object()) {
      if (const auto text = block.find("text"); text != block.end() && text->is_string()) {
        return text->get<std::string>();
      }
    }
  }
  return std::nullopt;
}

// Every `*.jsonl` under `dir`, recursing the YYYY/MM/DD layout Codex writes
// its rollouts into.
std::vector<std::filesystem::path> allRolloutFiles(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return out;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
      out.push_back(entry.path());
    }
  }
  return out;
}

// Line 1's session id (payload.id) when it is a session_meta record whose cwd
// matches `work_dir`; nullopt otherwise (the cheap reject for a file that
// isn't even this cwd's, before the caller scans the rest of it).
std::optional<std::string> sessionIdIfOurs(const std::string& first_line, const std::string& work_dir) {
  if (first_line.empty()) {
    return std::nullopt;
  }
  const json rec = json::parse(first_line, nullptr, /*allow_exceptions=*/false);
  if (!rec.is_object() || rec.value("type", std::string{}) != "session_meta") {
    return std::nullopt;
  }
  const auto& payload = rec.value("payload", json::object());
  if (!payload.is_object() || payload.value("cwd", std::string{}) != work_dir) {
    return std::nullopt;
  }
  const std::string id = payload.value("id", std::string{});
  return id.empty() ? std::nullopt : std::make_optional(id);
}

// The file whose name ends with `-<id>.jsonl`, or an empty path. Walks the
// directory directly (unlike the listing path) so it can return on the first
// match instead of building the full file list first.
std::filesystem::path findRolloutFileById(const std::filesystem::path& sessions_dir, const std::string& id) {
  const std::string suffix = "-" + id + ".jsonl";
  std::error_code ec;
  if (!std::filesystem::exists(sessions_dir, ec) || ec) {
    return {};
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           sessions_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return entry.path();
    }
  }
  return {};
}

}  // namespace

std::filesystem::path codexSessionsDir() {
  std::string base;
  if (const char* codex_home = std::getenv("CODEX_HOME"); codex_home != nullptr && codex_home[0] != '\0') {
    base = codex_home;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    base = std::string(home) + "/.codex";
  } else {
    return {};
  }
  return std::filesystem::path(base) / "sessions";
}

std::vector<ConversationSummary> listCodexConversations(
    const std::filesystem::path& sessions_dir, const std::string& work_dir) {
  std::vector<ConversationSummary> out;
  for (const auto& path : allRolloutFiles(sessions_dir)) {
    std::ifstream file(path);
    if (!file) {
      continue;
    }
    std::string line;
    if (!std::getline(file, line)) {
      continue;
    }
    const std::optional<std::string> id = sessionIdIfOurs(line, work_dir);
    if (!id.has_value()) {
      continue;
    }

    ConversationSummary summary;
    summary.id = *id;
    std::optional<std::string> first_user_text;

    // Same timestamp/type treatment for every record, including line 1
    // (already read above to resolve `id`) -- so the file is opened and
    // each line parsed exactly once.
    const auto scanLine = [&](const std::string& record_line) {
      if (record_line.empty()) {
        return;
      }
      const json rec = json::parse(record_line, nullptr, /*allow_exceptions=*/false);
      if (!rec.is_object()) {
        return;  // corrupt line: skip it, keep reading the rest of the file
      }
      if (const auto ts = rec.find("timestamp"); ts != rec.end() && ts->is_string()) {
        const std::string& t = ts->get_ref<const std::string&>();
        if (summary.first_ts.empty()) {
          summary.first_ts = t;
        }
        summary.last_ts = t;  // records are appended in order; last write wins
      }
      if (rec.value("type", std::string{}) != "response_item") {
        return;  // session_meta / event_msg / world_state / ...: not a message
      }
      const auto& payload = rec.value("payload", json::object());
      if (!payload.is_object() || payload.value("type", std::string{}) != "message") {
        return;  // reasoning / function_call / ...: not a message either
      }
      const std::string role = payload.value("role", std::string{});
      if (role == "assistant") {
        ++summary.assistant_messages;
      } else if (role == "user" && !first_user_text.has_value()) {
        if (const auto content = payload.find("content"); content != payload.end()) {
          if (const auto text = firstTextBlock(*content); text.has_value() && !isHarnessInjectedUserText(*text)) {
            first_user_text = text;
          }
        }
      }
      // "developer": harness-injected, SKIP (never a title/transcript source).
    };

    scanLine(line);
    while (std::getline(file, line)) {
      scanLine(line);
    }

    if (summary.assistant_messages == 0) {
      continue;  // a cancelled turn or a probe run against this cwd, not a real conversation
    }
    if (first_user_text.has_value()) {
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

std::vector<ChatMessage> loadCodexTranscript(const std::filesystem::path& sessions_dir, const std::string& id) {
  std::vector<ChatMessage> out;
  const std::filesystem::path found = findRolloutFileById(sessions_dir, id);
  if (found.empty()) {
    return out;
  }
  std::ifstream file(found);
  if (!file) {
    return out;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    const json rec = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!rec.is_object() || rec.value("type", std::string{}) != "response_item") {
      continue;
    }
    const auto& payload = rec.value("payload", json::object());
    if (!payload.is_object() || payload.value("type", std::string{}) != "message") {
      continue;
    }
    const std::string role = payload.value("role", std::string{});
    const auto content_it = payload.find("content");
    if (content_it == payload.end()) {
      continue;
    }
    const json& content = *content_it;

    if (role == "user") {
      if (const auto text = firstTextBlock(content); text.has_value() && !isHarnessInjectedUserText(*text)) {
        out.push_back({ChatMessage::Role::User, stripCatalogPrefix(*text)});
      }
      continue;  // an injected context message carries no row of its own
    }
    if (role != "assistant" || !content.is_array()) {
      continue;  // "developer": harness-injected, SKIP
    }
    for (const json& block : content) {
      if (!block.is_object() || block.value("type", std::string{}) != "output_text") {
        continue;
      }
      const std::string text = block.value("text", std::string{});
      if (!text.empty()) {
        out.push_back({ChatMessage::Role::Assistant, text});
      }
    }
  }
  return out;
}

bool deleteCodexConversation(const std::filesystem::path& sessions_dir, const std::string& id) {
  const std::filesystem::path found = findRolloutFileById(sessions_dir, id);
  if (found.empty()) {
    return true;  // already gone: a no-op success, like claude_sessions' deleteConversation
  }
  std::error_code ec;
  std::filesystem::remove(found, ec);
  return !ec;
}

}  // namespace assistant_agent
