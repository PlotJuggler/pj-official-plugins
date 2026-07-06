// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "tool_registry.hpp"

namespace assistant_agent {

// One thing that happens during a turn, emitted by a backend and relayed to the
// GUI thread (via the dialog's event queue) where it mutates the ChatSession.
struct BackendEvent {
  enum class Kind {
    AssistantText,  // a chunk of assistant prose to append
    ToolActivity,   // a human-readable note about a tool the model invoked
    Error,          // the turn failed; `text` is the reason
    TurnComplete,   // the backend is done; return the panel to Idle
  };
  Kind kind;
  std::string text;
};

// The tool surface a backend gets for the duration of one turn: the catalog to
// advertise to the model, and an invoker that runs a chosen tool (blocking the
// worker thread until the GUI thread executes it — see GuiExecutor).
struct TurnTools {
  const ToolRegistry* registry = nullptr;
  ToolInvoker invoke;
};

// Result of a backend connectivity probe (Ollama endpoint reachable, claude CLI
// present, …). `ok` false carries a human-readable reason for the transcript.
struct BackendTestResult {
  bool ok = false;
  std::string message;
};

// The backend seam. Implementations run entirely on the dialog's worker thread:
// they must NOT call host services directly. To run a tool they go through
// `tools.invoke`, which marshals to the GUI thread. The sink is invoked from the
// worker thread; the dialog's sink is the only place that crosses back to the
// GUI thread.
class LlmBackend {
 public:
  virtual ~LlmBackend() = default;

  using EventSink = std::function<void(BackendEvent)>;

  // Drive one user message to completion, emitting events through `sink` and
  // finishing with a single TurnComplete (even on error, which is reported as a
  // preceding Error event). Blocking; the worker thread owns the call.
  virtual void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) = 0;

  // Request cooperative cancellation of an in-flight turn. May be called from
  // the GUI thread while sendUserMessage runs on the worker thread, so
  // implementations must treat their cancel flag as cross-thread.
  virtual void cancel() = 0;

  // Human-readable backend name for the status line / transcript header.
  [[nodiscard]] virtual std::string name() const = 0;

  // Blocking connectivity probe, run on the worker thread after a settings
  // change. Backends with no meaningful check keep the default.
  [[nodiscard]] virtual BackendTestResult testConnection() const {
    return {true, "no connectivity test for this backend"};
  }
};

// M1 backend: echoes the user's message straight back. Ignores tools. Exercises
// the worker-thread -> event-queue -> transcript plumbing.
class EchoBackend : public LlmBackend {
 public:
  void sendUserMessage(const std::string& text, const TurnTools& /*tools*/, const EventSink& sink) override {
    cancelled_.store(false);
    sink({BackendEvent::Kind::AssistantText, "You said: " + text});
    sink({BackendEvent::Kind::TurnComplete, {}});
  }

  void cancel() override {
    cancelled_.store(true);
  }

  [[nodiscard]] std::string name() const override {
    return "Echo (no LLM)";
  }

 private:
  std::atomic<bool> cancelled_{false};
};

}  // namespace assistant_agent
