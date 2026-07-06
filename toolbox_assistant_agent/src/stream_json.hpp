// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace assistant_agent {

// Splits a byte stream into newline-delimited JSON records, buffering a partial
// trailing line across feeds. The `claude --output-format stream-json` stdout
// is one JSON object per line; a single read() may straddle line boundaries.
class NdjsonSplitter {
 public:
  void feed(const std::string& chunk, const std::function<void(const std::string&)>& on_line) {
    buffer_ += chunk;
    std::size_t start = 0;
    for (;;) {
      const std::size_t nl = buffer_.find('\n', start);
      if (nl == std::string::npos) {
        break;
      }
      const std::string line = buffer_.substr(start, nl - start);
      if (!line.empty()) {
        on_line(line);
      }
      start = nl + 1;
    }
    buffer_.erase(0, start);
  }

  // Flush any buffered trailing line without a newline (end of stream).
  void flush(const std::function<void(const std::string&)>& on_line) {
    if (!buffer_.empty()) {
      on_line(buffer_);
      buffer_.clear();
    }
  }

 private:
  std::string buffer_;
};

// One meaningful thing extracted from a claude stream-json record.
struct ClaudeEvent {
  enum class Kind {
    Init,           // session started; session_id set
    AssistantText,  // a text block from the assistant
    ToolUse,        // the model invoked a tool; tool_name set
    Result,         // the turn finished; text = final result, is_error set
    Ignored,        // a record we don't surface
  };
  Kind kind = Kind::Ignored;
  std::string text;
  std::string tool_name;
  std::string session_id;
  bool is_error = false;
};

// Parse one stream-json line into zero or more events. One assistant record can
// carry several content blocks (text + tool_use), so this returns a vector.
// Malformed JSON yields an empty vector (never throws).
[[nodiscard]] inline std::vector<ClaudeEvent> parseClaudeLine(const std::string& line) {
  std::vector<ClaudeEvent> out;
  auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    return out;
  }
  const std::string type = j.value("type", std::string{});
  const std::string session_id = j.value("session_id", std::string{});

  if (type == "system") {
    if (j.value("subtype", std::string{}) == "init") {
      ClaudeEvent e;
      e.kind = ClaudeEvent::Kind::Init;
      e.session_id = session_id;
      out.push_back(std::move(e));
    }
    return out;
  }

  if (type == "assistant" && j.contains("message") && j["message"].is_object()) {
    const auto& content = j["message"].value("content", nlohmann::json::array());
    if (content.is_array()) {
      for (const auto& block : content) {
        if (!block.is_object()) {
          continue;
        }
        const std::string btype = block.value("type", std::string{});
        if (btype == "text") {
          const std::string text = block.value("text", std::string{});
          if (!text.empty()) {
            ClaudeEvent e;
            e.kind = ClaudeEvent::Kind::AssistantText;
            e.text = text;
            e.session_id = session_id;
            out.push_back(std::move(e));
          }
        } else if (btype == "tool_use") {
          ClaudeEvent e;
          e.kind = ClaudeEvent::Kind::ToolUse;
          e.tool_name = block.value("name", std::string{});
          e.session_id = session_id;
          out.push_back(std::move(e));
        }
      }
    }
    return out;
  }

  if (type == "result") {
    ClaudeEvent e;
    e.kind = ClaudeEvent::Kind::Result;
    e.session_id = session_id;
    e.is_error = j.value("is_error", false) || j.value("subtype", std::string{}) != "success";
    // "result" carries the final assistant text on success; on error it may
    // carry an error string under the same key.
    e.text = j.value("result", std::string{});
    out.push_back(std::move(e));
    return out;
  }

  return out;
}

}  // namespace assistant_agent
