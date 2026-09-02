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
#include <vector>

#include "chat_session.hpp"
#include "claude_sessions.hpp"  // ConversationSummary
#include "gui_executor.hpp"
#include "llm_backend.hpp"
#include "tool_registry.hpp"
#include "usage_ledger.hpp"

namespace assistant_agent {

// Per-conversation state the backend borrows (claude_backend.hpp).
// Forward-declared so this header does not pull the backend implementation —
// and its JSON dependency — into everything that includes it; the dialog's
// destructor lives in the .cpp, where it is complete.
struct ClaudeMemory;

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

  // Human-readable active-backend name, shown on the status line.
  std::string backend_name = "Echo (no LLM)";

  // Dirty flags so widget_data() re-pushes only what changed. The transcript
  // re-push (setPlainText on a growing QPlainTextEdit) is the expensive one, so
  // it is gated behind an explicit change signal (risk R5).
  bool transcript_dirty = true;
  bool controls_dirty = true;
  // Clear the input box exactly once, right after a Send (never mid-typing).
  bool clear_input_pending = false;

  // What the last turn moved; rendered after statusText().
  UsageLedger usage;

  // The ☰ conversations drawer (left of the transcript). Populated from
  // backend_->listConversations() when opened — not kept live, so a
  // conversation started in another PlotJuggler instance only appears the
  // next time this one's drawer opens.
  bool drawer_open = false;
  bool drawer_dirty = true;          // forces the first widget_data() to push visibility + list + placeholder
  bool header_icons_pending = true;  // one-shot setButtonIconNamed for menuButton/newChatButton
  std::vector<ConversationSummary> conversations;
  std::string active_conversation_id;

  // A backend rebuild that arrived mid-turn and has to wait. Swapping the
  // backend is safe (the worker holds its own reference), but the conversation
  // memory is now SHARED between the outgoing and incoming objects, so doing it
  // while the worker is writing a session id is a data race. Applied by onTick
  // once the turn is over — outside the state lock, because rebuildBackend()
  // takes it and the mutex is not recursive.
  bool rebuild_pending = false;

  // Settings sub-dialog: request flag (read+cleared in widget_data) plus the
  // staged edits harvested from the modal's inputs on OK. PanelEngine fires
  // onTextChanged/onIndexChanged for each child, then onClicked("subDialogAccepted")
  // to commit — mirroring the toolbox_mosaico cert-dialog handshake.
  bool open_settings_pending = false;
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
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onItemDeleteRequested(std::string_view widget_name, int index) override;
  bool onTick() override;

  // Host wiring, mirroring toolbox_mosaico. Providers are captured lazily so the
  // tool layer can obtain a fresh view each call (on the GUI thread, in onTick).
  void setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider);
  void setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider);
  void setDataProcessorsProvider(std::function<PJ::sdk::DataProcessorsHostView()> provider);
  void setObjectReadProvider(std::function<PJ::sdk::ToolboxObjectReadHostView()> provider);
  void setPlaybackProvider(std::function<PJ::sdk::PlaybackHostView()> provider);
  void setViewportProvider(std::function<PJ::sdk::ViewportHostView()> provider);
  void setPlotTabsProvider(std::function<PJ::sdk::PlotTabHostView()> provider);
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
  // env override. Rebuilt whenever the backend choice changes. Defers itself
  // while a turn is in flight (see DialogState::rebuild_pending).
  void rebuildBackend();

  // Drop the transcript, the accumulated cost and the backend's conversation
  // memory — AND the active-session id in the settings store, so a later
  // reopen does not resume what the user explicitly walked away from. No
  // backend rebuild: the memory is cleared in place and the live backend
  // reads through the same pointer, so the next turn sends no --resume and
  // re-sends the catalog — genuinely fresh, with the MCP server left up. This
  // does NOT delete anything on disk: the old conversation stays in the
  // harness's store and in the drawer, just no longer active (New chat is not
  // the trash can — see onItemDeleteRequested for that).
  void startNewConversation();

  // Load the transcript for `id` from the backend's own store and replay it
  // into a cleared ChatSession via the SAME calls the live path uses
  // (addUser/appendAssistant/addTool), so a resumed transcript renders
  // identically to one built turn-by-turn — appendAssistant is what merges
  // consecutive assistant text blocks into one row. Points claude_memory_ at
  // `id` with resumed_pending set (composePayload then re-sends the catalog
  // with the resume note on the next turn) and persists the new active id. The
  // drawer stays as it was — a sidebar, not a menu. Returns false without
  // changing anything if the backend has nothing under `id` (purged between
  // listing and picking it).
  bool switchToConversation(const std::string& id);

  // Restore the active conversation once when the settings view is first
  // bound (the ctor runs before bind(), against an unbound view, so this
  // cannot run there). If the persisted id no longer resolves to anything
  // (purged since the last close), clears it instead of leaving a dangling
  // --resume target.
  void loadPersistedConversation();

  // Requires state_.mu held by the caller. Rebuilds `conversations` from
  // backend_->listConversations() — local disk I/O under the harness's
  // project dir, cheap enough to run inline on the GUI thread when the drawer
  // opens (there is no async path for it, unlike a turn).
  void refreshConversationsLocked();
  // Requires state_.mu held by the caller. The host's QListWidget protocol
  // selects and reports rows by TEXT (setSelectedItems / onSelectionChanged),
  // so these map a row's text back to its conversation id and forward; O(n)
  // over a human-sized list, not worth a map.
  [[nodiscard]] std::string idForListTextLocked(const std::string& text) const;
  [[nodiscard]] std::string listTextForIdLocked(const std::string& id) const;

  DialogState state_;
  ToolRegistry registry_;
  GuiExecutor gui_executor_;
  // shared_ptr, not unique_ptr: the worker thread captures its own reference
  // for the duration of a turn, so a Settings commit that rebuilds the backend
  // mid-turn swaps this member without destroying the object under the worker.
  std::shared_ptr<LlmBackend> backend_;
  // The conversation itself, outliving every backend built for it. Created once
  // in the constructor and lent to each backend, so changing the model — or
  // just saving the settings modal — no longer wipes what was said.
  std::shared_ptr<ClaudeMemory> claude_memory_;

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
  std::function<PJ::sdk::ToolboxObjectReadHostView()> object_read_provider_;
  std::function<PJ::sdk::PlaybackHostView()> playback_provider_;
  std::function<PJ::sdk::ViewportHostView()> viewport_provider_;
  std::function<PJ::sdk::PlotTabHostView()> plot_tabs_provider_;
  PJ::sdk::SettingsView settings_;
  // One restore per instance: setSettings can in principle be re-bound, and a
  // second load would duplicate the transcript on top of the live one.
  bool conversation_loaded_ = false;
};

}  // namespace assistant_agent
