// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <thread>

#include "chat_session.hpp"
#include "gui_executor.hpp"
#include "llm_backend.hpp"
#include "tool_registry.hpp"

namespace assistant_agent {

// DialogState — pure data the panel drives. Mutated on the GUI thread only
// (widget events + worker results drained by onTick), serialized into
// WidgetData on every widget_data(). Guarded by `mu` because worker-posted
// event closures run on the GUI thread but the fields are also read there;
// the lock keeps a single consistent snapshot per render.
struct DialogState {
  std::mutex mu;

  // Current text in the input box (mirrored from onTextChanged). Sent — and
  // cleared — when the user clicks Send.
  std::string input_text;

  // Transcript + turn-state machine (see chat_session.hpp).
  ChatSession session;

  // Human-readable active-backend name, shown in the header band.
  std::string backend_name = "Echo (no LLM)";

  // Dirty flags so widget_data() re-pushes only what changed. The transcript
  // re-push (setPlainText on a growing QPlainTextEdit) is the expensive one, so
  // it is gated behind an explicit change signal (risk R5).
  bool transcript_dirty = true;
  bool header_dirty = true;
  bool controls_dirty = true;
  // Clear the input box exactly once, right after a Send (never mid-typing).
  bool clear_input_pending = false;

  // Settings sub-dialog: request flag (read+cleared in widget_data) plus the
  // staged edits harvested from the modal's inputs on OK. PanelEngine fires
  // onTextChanged/onIndexChanged for each child, then onClicked("subDialogAccepted")
  // to commit — mirroring the toolbox_mosaico cert-dialog handshake.
  bool open_settings_pending = false;
  std::optional<std::string> pending_backend;  // "ollama" | "claude"
  std::optional<std::string> pending_ollama_url;
  std::optional<std::string> pending_ollama_model;
  std::optional<std::string> pending_claude_model;
  std::optional<std::string> pending_claude_cli;
};

// The chat panel. A non-modal DialogPluginTyped whose input drives a worker
// thread; results hop back through an event queue drained in onTick(). M1 wires
// an EchoBackend to prove the plumbing before real backends land.
class AssistantDialog : public PJ::DialogPluginTyped {
 public:
  AssistantDialog();
  ~AssistantDialog() override;

  // DialogPluginTyped overrides.
  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onClicked(std::string_view widget_name) override;
  bool onTick() override;

  // Host wiring, mirroring toolbox_mosaico. Providers are captured lazily so the
  // tool layer can obtain a fresh view each call (on the GUI thread, in onTick).
  void setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider);
  void setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider);
  void setDataProcessorsProvider(std::function<PJ::sdk::DataProcessorsHostView()> provider);
  void setSettings(PJ::sdk::SettingsView settings);

 private:
  void workerLoop();
  void postCommand(std::function<void()> fn);
  void postEvent(std::function<void()> fn);

  // GUI thread: take input_text, append it as a user turn, flip to
  // WaitingForLlm, and enqueue the backend call on the worker thread.
  void sendCurrentInput();

  // Apply a BackendEvent to the ChatSession. Runs on the GUI thread (posted by
  // the worker's sink through the event queue).
  void applyBackendEvent(const BackendEvent& ev);

  // Forward a one-shot message to the app's notification dropdown. Safe no-op

  // Persist staged settings edits committed by the settings sub-dialog.
  void commitSettings();
  // Build a ToolContext from the currently-bound host providers (GUI thread).
  ToolContext makeToolContext();
  // Select the backend implementation from settings + the ASSISTANT_FAKE_BACKEND
  // env override. Rebuilt whenever the backend choice changes.
  void rebuildBackend();

  DialogState state_;
  ToolRegistry registry_;
  GuiExecutor gui_executor_;
  // shared_ptr, not unique_ptr: the worker thread captures its own reference
  // for the duration of a turn, so a Settings commit that rebuilds the backend
  // mid-turn swaps this member without destroying the object under the worker.
  std::shared_ptr<LlmBackend> backend_;

  std::thread worker_thread_;
  std::mutex cmd_mu_;
  std::condition_variable cmd_cv_;
  std::deque<std::function<void()>> cmd_queue_;
  bool worker_stop_ = false;

  std::mutex evt_mu_;
  std::deque<std::function<void()>> evt_queue_;

  std::function<PJ::sdk::ToolboxHostView()> host_provider_;
  std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider_;
  std::function<PJ::sdk::DataProcessorsHostView()> dp_provider_;
  PJ::sdk::SettingsView settings_;
};

}  // namespace assistant_agent
