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
  // Only meaningful on Metrics. Carried as numbers rather than a formatted
  // string so the panel — not the backend — decides how to present them; a
  // backend whose provider reports no cost simply never emits the event.
  // Explicitly brace-initialized so the many `sink({Kind::X, "text"})` call
  // sites stay legal under -Werror=missing-field-initializers.
  TurnMetrics metrics{};
  // Error only: the turn failed because the conversation it tried to resume
  // no longer exists in the harness (purged, or deleted underneath us). The
  // panel tells the user retrying is pointless; every other error stays
  // exactly what it says.
  bool resume_failed = false;
  // ToolResult only: PlotJuggler's server (or the CLI) reported the call as an error. The panel does not
  // render the event; it is how a caller learns a call succeeded (ToolActivity is emitted before it runs).
  bool tool_failed = false;
};

// What a backend gets for the duration of one turn: where the CLI finds its
// tools, and the listings that ride ahead of the user's message.
struct TurnTools {
  // This plugin's endpoint on PlotJuggler's MCP server, acquired by the dialog
  // on the GUI thread before the turn (pj.mcp_endpoint.v1 is main-thread
  // only). The CLI calls the tools there directly; PlotJuggler runs them. An
  // error when this PlotJuggler cannot serve them: the backend then fails the
  // turn with that text, as is, before looking up its CLI. A copy, so the
  // worker shares nothing with the GUI thread. The token is a credential: it
  // reaches the CLI through a 0600 file or the child's environment, never argv.
  PJ::Expected<PJ::sdk::McpEndpoint> mcp = PJ::unexpected("no MCP endpoint was provided for this turn");

  // What data is currently loaded, in plain text (see catalogDigest). Handing
  // this over up front is what lets the model act without first spending
  // round-trips asking what exists. Where it goes is the backend's call,
  // because "once per conversation" means different things to each: a backend
  // that carries its own history injects it on the first turn only, while one
  // that rebuilds the message list every turn has to include it every time.
  std::string catalog;

  // What PlotJuggler offers (built-in features, plugins loaded, installable,
  // known limitations), rendered on the GUI thread by featuresBlock
  // (features_digest.hpp). Goes ahead of `catalog` in the message, under its
  // own dedup hash (harness_memory.hpp). Empty means no block at all.
  std::string features;
};

// Result of a backend connectivity probe (claude CLI present, a harness
// reachable, …). `ok` false carries a human-readable reason for the transcript.
struct BackendTestResult {
  bool ok = false;
  std::string message;
};

// The backend seam. Implementations run entirely on the dialog's worker thread:
// they must NOT call host services directly. Tools are not theirs to run: the
// CLI they spawn calls PlotJuggler's MCP server (TurnTools::mcp). The sink is
// invoked from the worker thread; the dialog's sink is the only place that
// crosses back to the GUI thread.
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

  // The models worth offering in the settings combo, beyond "CLI default" and
  // "Custom...", which every backend gets for free (assistant_dialog.cpp adds
  // them). Empty by default: a backend with nothing better to say (Echo) just
  // offers those two.
  [[nodiscard]] virtual std::vector<ModelChoice> availableModels() const {
    return {};
  }

  // Conversations this backend can list/resume/discard, newest first. A
  // conversation belongs to the backend that owns its store (ClaudeBackend
  // reads the Claude Code harness's own session files — claude_sessions.hpp);
  // a backend with no such store (Echo) simply has none, which is what
  // the empty defaults below say. Called on the GUI thread (opening the
  // conversations drawer, or resuming one), never mid-turn.
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

// Echoes the user's message straight back, for a backend key this build does
// not know. Needs no tools. Exercises the worker-thread -> event-queue ->
// transcript plumbing.
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
