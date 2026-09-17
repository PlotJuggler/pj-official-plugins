// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "turn_metrics.hpp"

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
    Result,         // the turn finished; text = final result, is_error set, metrics filled
    RateLimit,      // usage-window notice; text = status ("allowed", …)
    Ignored,        // a record we don't surface
  };
  Kind kind = Kind::Ignored;
  std::string text;
  std::string tool_name;
  std::string session_id;
  bool is_error = false;
  // Result only: how many model turns ran. 0 on an error means the CLI never
  // reached the model — with --resume, that the session it was asked to
  // continue does not exist. -1 when the record does not say.
  int num_turns = -1;
  TurnMetrics metrics;
  // Result only: the CLI's own "why did this turn end" tag, e.g. "api_error"
  // when it could not reach the API at all (verified: paired with an
  // all-zero usage block, with neither is_error nor subtype set on the real
  // line -- is_error above is derived from this pairing, see parseClaudeLine).
  // Empty when the record carries none.
  std::string terminal_reason;
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

  // The usage window is announced out of band. Surfacing it lets a long batch
  // stop deliberately instead of turning into a run of confusing failures.
  if (type == "rate_limit_event") {
    ClaudeEvent e;
    e.kind = ClaudeEvent::Kind::RateLimit;
    e.session_id = session_id;
    const auto& info = j.value("rate_limit_info", nlohmann::json::object());
    e.text = info.is_object() ? info.value("status", std::string{}) : std::string{};
    // The status is a family, not a flag: "allowed" and "allowed_warning" both
    // mean requests still go through — the second is only a heads-up that the
    // window is filling. Treating any non-"allowed" value as a block stops a
    // run that could have kept going, which is exactly what happened here.
    e.is_error = !e.text.empty() && e.text.rfind("allowed", 0) != 0;
    out.push_back(std::move(e));
    return out;
  }

  // The final record. `subtype` is absent on some builds, so only treat it as a
  // failure signal when it is present and not "success" — otherwise every turn
  // would be reported as an error.
  if (type == "result" || (type.empty() && j.contains("total_cost_usd"))) {
    ClaudeEvent e;
    e.kind = ClaudeEvent::Kind::Result;
    e.session_id = session_id;
    e.terminal_reason = j.value("terminal_reason", std::string{});
    const std::string subtype = j.value("subtype", std::string{});
    if (const auto& turns = j.value("num_turns", nlohmann::json{}); turns.is_number_integer()) {
      e.num_turns = turns.get<int>();
    }
    // "result" carries the final assistant text on success; on error it may
    // carry an error string under the same key (or nothing at all: a missing
    // --resume session yields `"result": null`).
    if (const auto& result = j.value("result", nlohmann::json{}); result.is_string()) {
      e.text = result.get<std::string>();
    }

    TurnMetrics m;
    m.cost_usd = j.value("total_cost_usd", 0.0);
    m.api_ms = j.value("duration_api_ms", 0);
    if (const auto& u = j.value("usage", nlohmann::json::object()); u.is_object()) {
      m.input_tokens = u.value("input_tokens", 0);
      m.output_tokens = u.value("output_tokens", 0);
      m.cache_read_tokens = u.value("cache_read_input_tokens", 0);
      m.cache_creation_tokens = u.value("cache_creation_input_tokens", 0);
    }
    m.valid = m.cost_usd > 0.0 || m.api_ms > 0 || m.output_tokens > 0;
    e.metrics = m;
    // terminal_reason=="api_error" paired with an all-zero (invalid) usage
    // block means the CLI never reached the API at all -- verified live: an
    // unrecognized --model value produces exactly this shape, and neither
    // is_error nor subtype says so on that real line. Folded into is_error
    // here, same precedent as the rate-limit event's status family above, so
    // callers (ClaudeBackend) only have to react to is_error + terminal_reason,
    // not re-derive the "did we even reach the API" condition themselves.
    e.is_error = j.value("is_error", false) || (!subtype.empty() && subtype != "success") ||
                 (e.terminal_reason == "api_error" && !m.valid);
    out.push_back(std::move(e));
    return out;
  }

  return out;
}

}  // namespace assistant_agent
