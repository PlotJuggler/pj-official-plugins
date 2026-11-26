// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <pj_base/expected.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>  // McpEndpoint
#include <string>
#include <vector>

#include "chat_session.hpp"     // ChatMessage
#include "claude_sessions.hpp"  // ConversationSummary
#include "model_choice.hpp"     // ModelChoice
#include "turn_metrics.hpp"

namespace assistant_agent {

// One thing that happens during a turn, emitted by a backend and relayed to the
// GUI thread (via the dialog's event queue) where it mutates the ChatSession.
struct BackendEvent {
  enum class Kind {
    AssistantText,  // a chunk of assistant prose to append
    ToolActivity,   // a human-readable note about a tool the model invoked
    ToolResult,     // a tool the model invoked finished; `text` names it, `tool_failed` says how
    Error,          // the turn failed; `text` is the reason
    Metrics,        // what the turn cost; `metrics` is filled, `text` unused
    TurnComplete,   // the backend is done; return the panel to Idle
  };
  Kind kind{};
  std::string text;
  // Metrics only. Numbers, not a formatted string, so the panel decides how to present
  // them. Brace-initialized so `sink({Kind::X, "text"})` stays legal under
  // -Werror=missing-field-initializers.
  TurnMetrics metrics{};
  // Error only: the conversation being resumed no longer exists in the harness (purged
  // or deleted), so the panel tells the user retrying is pointless.
  bool resume_failed = false;
  // ToolResult only: PlotJuggler's server (or the CLI) reported the call as an error.
  // The panel does not render the event; it is how a caller learns a call succeeded
  // (ToolActivity is emitted before the call runs).
  bool tool_failed = false;
};

// What a backend gets for one turn: where the CLI finds its tools, and the listings
// that ride ahead of the user's message.
struct TurnTools {
  // This plugin's endpoint on PlotJuggler's MCP server, acquired on the GUI thread
  // before the turn (pj.mcp_endpoint.v1 is main-thread only); the CLI calls the tools
  // there directly. An error means this PlotJuggler cannot serve them: the backend fails
  // the turn with that text before looking up its CLI. A copy, so the worker shares
  // nothing with the GUI thread. The token is a credential: it reaches the CLI through
  // a 0600 file or the child's environment, never argv.
  PJ::Expected<PJ::sdk::McpEndpoint> mcp = PJ::unexpected("no MCP endpoint was provided for this turn");

  // Loaded data in plain text (see catalogDigest), so the model need not spend
  // round-trips asking what exists. Where it goes is the backend's call: one that
  // carries its own history injects it on the first turn only, one that rebuilds the
  // message list every turn must include it every time.
  std::string catalog;

  // What PlotJuggler offers (features, plugins, limitations), rendered on the GUI
  // thread by featuresBlock. Goes ahead of `catalog` under its own dedup hash
  // (harness_memory.hpp). Empty means no block.
  std::string features;
};

// Result of a backend connectivity probe; `ok` false carries a reason for the transcript.
struct BackendTestResult {
  bool ok = false;
  std::string message;
};

// The backend seam. Implementations run on the dialog's worker thread and must NOT call
// host services directly: the CLI they spawn calls PlotJuggler's MCP server
// (TurnTools::mcp). The sink is invoked from the worker thread; the dialog's sink is the
// only place that crosses back to the GUI thread.
class LlmBackend {
 public:
  virtual ~LlmBackend() = default;

  using EventSink = std::function<void(BackendEvent)>;

  // Blocking. Emits events through `sink` and finishes with a single TurnComplete, even
  // on error (reported as a preceding Error event).
  virtual void sendUserMessage(const std::string& text, const TurnTools& tools, const EventSink& sink) = 0;

  // Cooperative cancellation; called from the GUI thread while sendUserMessage runs on
  // the worker, so the cancel flag must be cross-thread safe.
  virtual void cancel() = 0;

  [[nodiscard]] virtual std::string name() const = 0;

  // Blocking; run on the worker thread after a settings change.
  [[nodiscard]] virtual BackendTestResult testConnection() const {
    return {true, "no connectivity test for this backend"};
  }

  // Models for the settings combo beyond "CLI default" and "Custom...", which
  // assistant_dialog.cpp adds for every backend.
  [[nodiscard]] virtual std::vector<ModelChoice> availableModels() const {
    return {};
  }

  // Conversations this backend can list/resume/discard, newest first; a backend with no
  // store of its own (Echo) has none. Called on the GUI thread, never mid-turn.
  [[nodiscard]] virtual std::vector<ConversationSummary> listConversations() {
    return {};
  }
  [[nodiscard]] virtual std::vector<ChatMessage> loadTranscript(const std::string& /*id*/) {
    return {};
  }
  virtual bool deleteConversation(const std::string& /*id*/) {
    return false;
  }
};

// Echoes the user's message back, for a backend key this build does not know. Needs no tools.
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
