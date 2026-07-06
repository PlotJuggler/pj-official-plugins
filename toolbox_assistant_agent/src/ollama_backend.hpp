// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <string>

#include "llm_backend.hpp"

namespace assistant_agent {

// Local LLM backend driving Ollama's native /api/chat with stream:false. It
// runs the agentic tool loop in-plugin: advertise the ToolRegistry as Ollama
// tools, and on each tool_calls round invoke the tool (through the GuiExecutor)
// and feed the result back until the model answers with prose. Non-streaming is
// intentional — the panel re-pushes the whole transcript, so there is nothing to
// stream into.
class OllamaBackend : public LlmBackend {
 public:
  OllamaBackend(std::string base_url, std::string model);

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
  std::atomic<bool> cancel_{false};
};

}  // namespace assistant_agent
