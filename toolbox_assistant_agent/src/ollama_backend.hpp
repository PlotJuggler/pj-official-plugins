// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "llm_backend.hpp"

namespace assistant_agent {

// Everything said so far, in Ollama's message format — the system message, then
// user/assistant/tool turns in order. Owned by the caller rather than by the
// backend for the same reason as ClaudeMemory: changing the endpoint or the
// model rebuilds the backend, and the user did not ask to be forgotten.
struct OllamaMemory {
  nlohmann::json history = nlohmann::json::array();
};

// Local LLM backend driving Ollama's native /api/chat. It runs the agentic tool
// loop in-plugin: advertise the ToolRegistry as Ollama tools, and on each
// tool_calls round invoke the tool (through the GuiExecutor) and feed the result
// back until the model answers with prose.
//
// Streams: chunks are emitted as they arrive so the panel fills in while the
// model is still generating, instead of sitting on "Thinking..." until the whole
// answer is ready. Ollama's stream is NDJSON, one JSON object per line.
//
// Conversation state lives in `memory_`. Unlike the Claude backend — which
// delegates continuity to the CLI's own session via --resume — this backend
// owns the message list, so anything not kept there is forgotten between turns.
class OllamaBackend : public LlmBackend {
 public:
  // `memory` is lent by the dialog and outlives any single backend; null gets a
  // fresh private one, which is what the tests want.
  OllamaBackend(std::string base_url, std::string model, std::shared_ptr<OllamaMemory> memory = nullptr);

  void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) override;
  void cancel() override;
  [[nodiscard]] std::string name() const override;

  // Connectivity probe for the settings flow: GET <base_url>/api/tags.
  [[nodiscard]] BackendTestResult testConnection() const override;

 private:
  // The agentic turn loop; sendUserMessage wraps it in the worker-thread
  // exception barrier (untrusted server JSON + throwing typed getters).
  void runTurn(const std::string& text, const TurnTools& tools, const EventSink& sink);

 private:
  std::string base_url_;  // e.g. "http://localhost:11434" (no trailing slash)
  std::string model_;
  std::shared_ptr<OllamaMemory> memory_;  // never null after construction
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
