// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "turn_metrics.hpp"

namespace assistant_agent {

// One meaningful thing extracted from a `codex exec --json` stdout record.
// Unlike Claude's stream-json (one record can carry several content blocks),
// each Codex line maps to exactly one event, so this parser returns a single
// CodexEvent rather than a vector (see stream_json.hpp for the Claude shape).
struct CodexEvent {
  enum class Kind {
    ThreadStarted,    // "thread.started"; session_id set
    TurnStarted,      // "turn.started"
    AssistantText,    // item.completed, item.type == "agent_message"; text set
    ToolCallStarted,  // item.started, item.type == "mcp_tool_call"; tool_name set
    ToolCallFailed,   // item.completed, item.type == "mcp_tool_call" with a non-null error; tool_name + text set
    ErrorItem,        // item.completed item.type == "error", OR a top-level {"type":"error",...}; text set
    TurnCompleted,    // "turn.completed"; metrics filled
    TurnFailed,       // "turn.failed"; text set
    Ignored,          // a record we don't surface (including a completed tool call with no error)
  };
  Kind kind = Kind::Ignored;
  std::string text;        // AssistantText / ErrorItem / TurnFailed
  std::string tool_name;   // ToolCallStarted / ToolCallFailed
  std::string session_id;  // ThreadStarted only
  TurnMetrics metrics;     // TurnCompleted only
};

// Parse one `codex exec --json` stdout line. Malformed JSON, or a shape this
// build has never heard of, yields Kind::Ignored — never throws. Whether an
// ErrorItem is actually a failure is the BACKEND's call, not the parser's: a
// startup notice ("Code Mode is unavailable...") arrives as the same
// item.completed/error shape as a genuine mid-turn failure, and the only thing
// that tells them apart is whether turn.started has been seen yet
// (codex_backend.cpp's `in_turn`).
[[nodiscard]] inline CodexEvent parseCodexLine(const std::string& line) {
  CodexEvent out;
  auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    return out;
  }
  const std::string type = j.value("type", std::string{});

  if (type == "thread.started") {
    out.kind = CodexEvent::Kind::ThreadStarted;
    out.session_id = j.value("thread_id", std::string{});
    return out;
  }
  if (type == "turn.started") {
    out.kind = CodexEvent::Kind::TurnStarted;
    return out;
  }
  if (type == "item.started") {
    const auto& item = j.value("item", nlohmann::json::object());
    if (item.is_object() && item.value("type", std::string{}) == "mcp_tool_call") {
      out.kind = CodexEvent::Kind::ToolCallStarted;
      out.tool_name = item.value("tool", std::string{});
    }
    return out;
  }
  if (type == "item.completed") {
    const auto& item = j.value("item", nlohmann::json::object());
    if (!item.is_object()) {
      return out;
    }
    const std::string item_type = item.value("type", std::string{});
    if (item_type == "mcp_tool_call") {
      const auto& error = item.value("error", nlohmann::json{});
      if (error.is_object()) {
        out.kind = CodexEvent::Kind::ToolCallFailed;
        out.tool_name = item.value("tool", std::string{});
        out.text = error.value("message", std::string{});
      }
      // A completed call with no error carries no event of its own: the
      // model's next turn sees the result as part of its own context, exactly
      // like Claude's tool_result blocks (claude_backend.cpp never surfaces
      // those either — only the ToolUse that started the call).
      return out;
    }
    if (item_type == "error") {
      out.kind = CodexEvent::Kind::ErrorItem;
      out.text = item.value("message", std::string{});
      return out;
    }
    if (item_type == "agent_message") {
      out.kind = CodexEvent::Kind::AssistantText;
      out.text = item.value("text", std::string{});
      return out;
    }
    return out;
  }
  if (type == "turn.completed") {
    out.kind = CodexEvent::Kind::TurnCompleted;
    const auto& usage = j.value("usage", nlohmann::json::object());
    TurnMetrics m;
    m.cost_usd = 0.0;  // Codex reports no price, only tokens
    m.input_tokens = usage.value("input_tokens", 0);
    m.output_tokens = usage.value("output_tokens", 0);
    m.cache_read_tokens = usage.value("cached_input_tokens", 0);
    m.cache_creation_tokens = usage.value("cache_write_input_tokens", 0);
    m.api_ms = 0;  // Codex reports no wall-clock split
    m.valid = m.input_tokens > 0 || m.output_tokens > 0;
    out.metrics = m;
    return out;
  }
  if (type == "turn.failed") {
    out.kind = CodexEvent::Kind::TurnFailed;
    out.text = j.value("error", nlohmann::json::object()).value("message", std::string{});
    return out;
  }
  if (type == "error") {
    out.kind = CodexEvent::Kind::ErrorItem;
    out.text = j.value("message", std::string{});
    return out;
  }
  return out;
}

}  // namespace assistant_agent
