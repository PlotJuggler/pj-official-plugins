// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "ollama_backend.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

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

  json messages = json::array();
  messages.push_back({{"role", "system"}, {"content", kSystemPrompt}});
  messages.push_back({{"role", "user"}, {"content", text}});

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

    json request = {{"model", model_}, {"messages", messages}, {"stream", false}};
    if (!tool_specs.empty()) {
      request["tools"] = tool_specs;
    }

    auto args = client.createRequest();
    args->extraHeaders["Content-Type"] = "application/json";
    args->connectTimeout = 10;
    args->transferTimeout = 600;
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

    auto reply = json::parse(resp->body, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object() || !reply.contains("message")) {
      sink({BackendEvent::Kind::Error, "unexpected Ollama response"});
      exhausted_rounds = false;
      break;
    }
    const json& msg = reply["message"];
    // Keep the assistant turn (incl. any tool_calls) in history so the model
    // knows which call each tool result answers.
    messages.push_back(msg);

    const std::string content = msg.value("content", std::string{});
    const json tool_calls = msg.value("tool_calls", json::array());

    if (tool_calls.is_array() && !tool_calls.empty()) {
      if (!content.empty()) {
        sink({BackendEvent::Kind::AssistantText, content});
      }
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

    // No tool calls -> this is the final answer.
    if (!content.empty()) {
      sink({BackendEvent::Kind::AssistantText, content});
    } else {
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
