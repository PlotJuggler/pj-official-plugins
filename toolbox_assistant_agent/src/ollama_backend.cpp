// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "ollama_backend.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "stream_json.hpp"
#include "system_prompt.hpp"

namespace assistant_agent {
namespace {

using nlohmann::json;

// Cap on agentic tool rounds per turn — a runaway model that keeps calling tools
// must terminate. 16 is comfortably more than any real task here needs.
constexpr int kMaxToolRounds = 16;

std::string stripTrailingSlash(std::string s) {
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

// Ollama returns tool-call arguments as a JSON object; some builds/models send a
// JSON string instead. Normalize to an object either way.
json normalizeArgs(const json& raw) {
  if (raw.is_object()) {
    return raw;
  }
  if (raw.is_string()) {
    return json::parse(raw.get<std::string>(), nullptr, /*allow_exceptions=*/false);
  }
  return json::object();
}

}  // namespace

OllamaBackend::OllamaBackend(std::string base_url, std::string model)
    : base_url_(stripTrailingSlash(std::move(base_url))), model_(std::move(model)) {}

std::string OllamaBackend::name() const {
  return "Ollama: " + (model_.empty() ? std::string("(no model set)") : model_);
}

void OllamaBackend::cancel() {
  cancel_.store(true);
}

BackendTestResult OllamaBackend::testConnection() const {
  if (base_url_.empty()) {
    return {false, "no Ollama URL configured"};
  }
  ix::HttpClient client(/*async=*/false);
  auto args = client.createRequest();
  args->connectTimeout = 5;
  args->transferTimeout = 10;
  auto resp = client.get(base_url_ + "/api/tags", args);
  if (resp == nullptr || resp->statusCode <= 0) {
    return {false, "cannot reach " + base_url_ + (resp ? ": " + resp->errorMsg : "")};
  }
  if (resp->statusCode != 200) {
    return {false, "Ollama returned HTTP " + std::to_string(resp->statusCode)};
  }
  auto j = json::parse(resp->body, nullptr, /*allow_exceptions=*/false);
  int models =
      (j.is_object() && j.contains("models") && j["models"].is_array()) ? static_cast<int>(j["models"].size()) : 0;
  return {true, "connected to Ollama (" + std::to_string(models) + " model(s) installed)"};
}

void OllamaBackend::sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) {
  // Runs on the worker thread: an escaped exception would std::terminate the
  // process. The server reply is untrusted JSON and the typed getters below
  // throw on wrong-typed fields, so fail the turn instead.
  try {
    runTurn(text, tools, sink);
  } catch (const std::exception& e) {
    sink({BackendEvent::Kind::Error, std::string("Ollama backend error: ") + e.what()});
    sink({BackendEvent::Kind::TurnComplete, {}});
  }
}

void OllamaBackend::runTurn(const std::string& text, const TurnTools& tools, const EventSink& sink) {
  cancel_.store(false);
  if (model_.empty()) {
    sink({BackendEvent::Kind::Error, "No Ollama model set — open Settings and enter one."});
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }

  // The system message is rebuilt every turn and kept at index 0, so a catalog
  // that changed (another file loaded, a stream started) is reflected without
  // discarding what has already been said.
  const std::string system =
      tools.catalog.empty() ? std::string(kSystemPrompt) : std::string(kSystemPrompt) + "\n\n" + tools.catalog;
  if (history_.empty()) {
    history_.push_back({{"role", "system"}, {"content", system}});
  } else {
    history_[0] = {{"role", "system"}, {"content", system}};
  }
  history_.push_back({{"role", "user"}, {"content", text}});
  json& messages = history_;

  const json tool_specs = (tools.registry != nullptr) ? tools.registry->toOllamaTools() : json::array();
  ix::HttpClient client(/*async=*/false);

  // True only if the loop runs its full budget without a final answer or an
  // early break — distinguishes "hit the round cap" from "errored/cancelled".
  bool exhausted_rounds = true;

  for (int round = 0; round < kMaxToolRounds; ++round) {
    if (cancel_.load()) {
      sink({BackendEvent::Kind::Error, "cancelled"});
      exhausted_rounds = false;
      break;
    }

    json request = {{"model", model_}, {"messages", messages}, {"stream", true}};
    if (!tool_specs.empty()) {
      request["tools"] = tool_specs;
    }

    // Streamed: emit each text delta as it lands so the panel fills in while the
    // model generates. The deltas ARE the answer — the accumulated `content`
    // below is only kept for the history entry, never re-emitted, or the whole
    // reply would appear twice in the transcript.
    NdjsonSplitter splitter;
    std::string content;
    json tool_calls = json::array();
    bool malformed = false;
    auto on_line = [&](const std::string& line) {
      auto obj = json::parse(line, nullptr, /*allow_exceptions=*/false);
      if (!obj.is_object() || !obj.contains("message")) {
        malformed = true;
        return;
      }
      const json& m = obj["message"];
      if (const std::string delta = m.value("content", std::string{}); !delta.empty()) {
        content += delta;
        sink({BackendEvent::Kind::AssistantText, delta});
      }
      // Tool calls arrive whole, in one chunk, rather than character by
      // character — collect them across the stream.
      if (auto it = m.find("tool_calls"); it != m.end() && it->is_array()) {
        for (const auto& tc : *it) {
          tool_calls.push_back(tc);
        }
      }
    };

    auto args = client.createRequest();
    args->extraHeaders["Content-Type"] = "application/json";
    args->connectTimeout = 10;
    args->transferTimeout = 600;
    args->onChunkCallback = [&](const std::string& chunk) { splitter.feed(chunk, on_line); };
    auto resp = client.post(base_url_ + "/api/chat", request.dump(), args);

    if (resp == nullptr || resp->statusCode <= 0) {
      sink({BackendEvent::Kind::Error, "cannot reach Ollama at " + base_url_ + (resp ? ": " + resp->errorMsg : "")});
      exhausted_rounds = false;
      break;
    }
    if (resp->statusCode != 200) {
      sink({BackendEvent::Kind::Error, "Ollama HTTP " + std::to_string(resp->statusCode) + ": " + resp->body});
      exhausted_rounds = false;
      break;
    }
    // The final record has no trailing newline, so it sits in the splitter's
    // buffer until flushed. A single-object (non-streaming) reply is entirely
    // that case, which is why this is not just an edge condition.
    splitter.flush(on_line);
    // And a server that ignores stream:true may return the whole body at once
    // without ever firing the chunk callback; parse it directly so those keep
    // working, just without the incremental display.
    if (content.empty() && tool_calls.empty() && !resp->body.empty()) {
      splitter.feed(resp->body, on_line);
      splitter.flush(on_line);
    }
    if (content.empty() && tool_calls.empty()) {
      sink({BackendEvent::Kind::Error, malformed ? "unexpected Ollama response" : "empty Ollama response"});
      exhausted_rounds = false;
      break;
    }

    // Keep the assistant turn (incl. any tool_calls) in history so the model
    // knows which call each tool result answers.
    json msg = {{"role", "assistant"}, {"content", content}};
    if (!tool_calls.empty()) {
      msg["tool_calls"] = tool_calls;
    }
    messages.push_back(msg);

    if (!tool_calls.empty()) {
      for (const auto& tc : tool_calls) {
        const json& fn = tc.value("function", json::object());
        const std::string tool_name = fn.value("name", std::string{});
        const json tool_args = normalizeArgs(fn.value("arguments", json::object()));
        sink({BackendEvent::Kind::ToolActivity, tool_name + "(" + tool_args.dump() + ")"});
        ToolResult result =
            tools.invoke ? tools.invoke(tool_name, tool_args) : ToolResult::failure("no tool invoker wired");
        messages.push_back({{"role", "tool"}, {"tool_name", tool_name}, {"content", result.content}});
      }
      continue;  // let the model read the tool results
    }

    // No tool calls -> this is the final answer, and it has already been sent
    // out delta by delta above. Re-emitting `content` here would print the whole
    // reply a second time.
    if (content.empty()) {
      sink({BackendEvent::Kind::AssistantText, "(no response)"});
    }
    sink({BackendEvent::Kind::TurnComplete, {}});
    return;
  }

  if (exhausted_rounds) {
    sink({BackendEvent::Kind::Error, "stopped after " + std::to_string(kMaxToolRounds) + " tool rounds"});
  }
  sink({BackendEvent::Kind::TurnComplete, {}});
}

}  // namespace assistant_agent
