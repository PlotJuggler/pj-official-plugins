// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "codex_sessions.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/text_utils.hpp>  // PJ::sdk::lowerAscii

#include "platform_util.hpp"  // getEnvUtf8, userHomeDir

namespace assistant_agent {

namespace {

using nlohmann::json;

// Compares a rollout's recorded `cwd` against `work_dir`. POSIX: exact match. Windows
// normalizes both sides, since one directory has several spellings: the `\\?\` long-path
// prefix is stripped, '/' becomes '\\', one trailing separator is dropped (never more:
// this plugin never produces a doubled one, and trimming more could accept a different
// directory), and the comparison is ASCII case-insensitive like NTFS.
bool sameCwd(const std::string& a, const std::string& b) {
#if defined(_WIN32)
  const auto normalize = [](const std::string& raw) {
    std::string s = raw;
    constexpr const char* kLongPathPrefix = R"(\\?\)";
    if (s.rfind(kLongPathPrefix, 0) == 0) {
      s = s.substr(std::strlen(kLongPathPrefix));
    }
    for (char& c : s) {
      if (c == '/') {
        c = '\\';
      }
    }
    if (s.size() > 1 && s.back() == '\\') {
      s.pop_back();
    }
    return s;
  };
  const std::string na = normalize(a);
  const std::string nb = normalize(b);
  return PJ::sdk::lowerAscii(na) == PJ::sdk::lowerAscii(nb);
#else
  return a == b;
#endif
}

// The opening text is the only signal; see codex_sessions.hpp.
bool isHarnessInjectedUserText(const std::string& text) {
  return text.rfind("<environment_context>", 0) == 0 || text.rfind("<recommended_plugins>", 0) == 0 ||
         text.rfind("# AGENTS.md instructions", 0) == 0;
}

// The first block with a "text" field in a response_item message's `content`, always
// an array of typed blocks ({"type":"input_text"/"output_text","text":...}).
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

// Line 1's session id (payload.id) when it is a session_meta record whose cwd matches
// `work_dir`; nullopt is the cheap reject before scanning the rest of the file.
std::optional<std::string> sessionIdIfOurs(const std::string& first_line, const std::string& work_dir) {
  if (first_line.empty()) {
    return std::nullopt;
  }
  const json rec = json::parse(first_line, nullptr, /*allow_exceptions=*/false);
  if (!rec.is_object() || rec.value("type", std::string{}) != "session_meta") {
    return std::nullopt;
  }
  const auto& payload = rec.value("payload", json::object());
  if (!payload.is_object() || !sameCwd(payload.value("cwd", std::string{}), work_dir)) {
    return std::nullopt;
  }
  const std::string id = payload.value("id", std::string{});
  return id.empty() ? std::nullopt : std::make_optional(id);
}

// The file whose name ends with `-<id>.jsonl`, or an empty path. Returns on the first
// match instead of building the full file list.
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

std::filesystem::path codexHomeDir() {
  if (const std::optional<std::string> codex_home = getEnvUtf8("CODEX_HOME")) {
    return utf8ToPath(*codex_home);
  }
  if (const std::optional<std::string> home = userHomeDir()) {
    return utf8ToPath(*home) / ".codex";
  }
  return {};
}

std::filesystem::path codexSessionsDir() {
  const std::filesystem::path base = codexHomeDir();
  return base.empty() ? std::filesystem::path{} : base / "sessions";
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

    // Line 1 (already read to resolve `id`) goes through here too, so each line is
    // parsed once.
    const auto scanLine = [&](const std::string& record_line) {
      if (record_line.empty()) {
        return;
      }
      const json rec = json::parse(record_line, nullptr, /*allow_exceptions=*/false);
      if (!rec.is_object()) {
        return;
      }
      if (const auto ts = rec.find("timestamp"); ts != rec.end() && ts->is_string()) {
        const std::string& t = ts->get_ref<const std::string&>();
        if (summary.first_ts.empty()) {
          summary.first_ts = t;
        }
        summary.last_ts = t;
      }
      if (rec.value("type", std::string{}) != "response_item") {
        return;
      }
      const auto& payload = rec.value("payload", json::object());
      if (!payload.is_object() || payload.value("type", std::string{}) != "message") {
        return;
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
      // "developer" records are harness-injected: never a title/transcript source.
    };

    scanLine(line);
    while (std::getline(file, line)) {
      scanLine(line);
    }

    if (summary.assistant_messages == 0) {
      continue;  // a cancelled turn or probe run, not a real conversation
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
    return a.last_ts > b.last_ts;  // ISO 8601 sorts lexicographically
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
      continue;
    }
    if (role != "assistant" || !content.is_array()) {
      continue;
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
    return true;  // already gone counts as success
  }
  std::error_code ec;
  std::filesystem::remove(found, ec);
  return !ec;
}

}  // namespace assistant_agent
