// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
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
#include "claude_sessions.hpp"     // ConversationSummary
#include "conversation_state.hpp"  // ConversationTitles
#include "llm_backend.hpp"
#include "usage_ledger.hpp"

namespace assistant_agent {

class AssistantDialogTestPeer;
class SettingsStore;

// Character budget for the catalog digest (catalogDigest), from settings key
// "assistant.catalog_budget_chars", clamped to [1000, 200000]; an absent or unparsable value
// falls back to catalogDigest's default (10000). Declared here so tests can drive it without a
// full dialog; read fresh every turn.
[[nodiscard]] int resolveCatalogBudgetChars(const SettingsStore& store);

// The same for the "PlotJuggler features" block: key "assistant.features_budget_chars",
// default kDefaultFeaturesBudgetChars, clamped to [kMinFeaturesBudgetChars,
// kMaxFeaturesBudgetChars] (the minimum holds the built-in reference, which is never trimmed).
[[nodiscard]] int resolveFeaturesBudgetChars(const SettingsStore& store);

// Per-conversation state a backend borrows (harness_memory.hpp). Forward-declared so this
// header does not pull in the backends and their JSON dependency; the dialog's destructor
// lives in the .cpp where it is complete.
struct HarnessMemory;

// Which sub-dialog PanelEngine's single synthetic "subDialogAccepted" click (fired for ANY
// accepted sub-dialog) refers to. Set when a sub-dialog is requested, routed and cleared at
// accept (onClicked), and also cleared by the next widget_data() with nothing newly pending:
// the backstop for a Cancel, which never fires subDialogAccepted.
enum class SubDialogKind { None, Settings, Rename };

// Pure data the panel drives, serialized into WidgetData on every widget_data(). Mutated on the
// GUI thread only (widget events and worker results drained by onTick). `mu` keeps a consistent
// snapshot per render.
struct DialogState {
  std::mutex mu;

  // Mirrored from onTextChanged; sent and cleared on Send.
  std::string input_text;

  ChatSession session;

  std::string backend_name = "Echo (no LLM)";

  // widget_data() re-pushes only what changed; the transcript re-push (setPlainText on a growing
  // transcript) is the expensive one.
  bool transcript_dirty = true;
  bool controls_dirty = true;
  // Clears the input box exactly once, right after a Send (never mid-typing).
  bool clear_input_pending = false;

  // Rendered after statusText().
  UsageLedger usage;

  // The conversations drawer (left of the transcript), populated from
  // backend_->listConversations() on bind, on a backend switch, after a delete and after every
  // completed turn.
  bool drawer_dirty = true;          // forces the first widget_data() to push the list + placeholder
  bool header_icons_pending = true;  // one-shot setButtonIconNamed for newChatButton
  std::vector<ConversationSummary> conversations;
  std::string active_conversation_id;
  // Custom names keyed by conversation id, refreshed with `conversations`
  // (refreshConversationsLocked).
  ConversationTitles conversation_titles;

  // A backend rebuild that arrived mid-turn. The conversation memory is SHARED between outgoing
  // and incoming backends, so swapping while the worker writes a session id is a data race.
  // Applied by onTick once the turn is over, outside the state lock because rebuildBackend()
  // takes it and the mutex is not recursive.
  bool rebuild_pending = false;

  // Which sub-dialog widget_data() last requested (see SubDialogKind).
  SubDialogKind open_sub_dialog = SubDialogKind::None;

  // Settings sub-dialog: request flag (read and cleared in widget_data) plus the staged edits
  // harvested on OK. PanelEngine fires onTextChanged/onIndexChanged per child, then
  // onClicked("subDialogAccepted") to commit.
  bool open_settings_pending = false;
  // Staged text edits keyed by SETTINGS key, not widget name (onTextChanged maps the widget via
  // each BackendSpec; e.g. "assistant.claude.model" -> typed value). commitSettings persists and
  // clears them.
  std::map<std::string, std::string> pending_text;
  // Staged backendCombo choice ("claude"/"codex"); commitSettings persists and clears it.
  std::optional<std::string> pending_backend;

  // Rename sub-dialog: same request/stage/commit shape, one field each (ui/rename_conversation.ui).
  bool open_rename_pending = false;
  // The conversation the context-menu action fired on, NOT necessarily active_conversation_id:
  // renaming never changes which conversation is open.
  std::string rename_conversation_id;
  // renameEdit's prefill (the row's current display name), sent when the sub-dialog is requested.
  std::string rename_prefill;
  // renameEdit's live text, staged by onTextChanged and committed on accept.
  std::optional<std::string> pending_rename_text;
};

// The chat panel: a non-modal DialogPluginTyped whose input drives a worker thread; results hop
// back through an event queue drained in onTick(). The agent CLI a backend spawns calls its
// tools on PlotJuggler's MCP server, at the endpoint each turn acquires from mcp_host_.
class AssistantDialog : public PJ::DialogPluginTyped {
 public:
  AssistantDialog();
  ~AssistantDialog() override;

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onClicked(std::string_view widget_name) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onItemDeleteRequested(std::string_view widget_name, int index) override;
  bool onItemContextAction(std::string_view widget_name, int index, std::string_view action_id) override;
  bool onTick() override;

  // Host wiring for the catalog digest, built on the GUI thread before every turn. An unbound
  // toolbox host means no digest; an unbound playback view, object time ranges in raw seconds.
  void setCatalogHosts(PJ::sdk::ToolboxHostView host, PJ::sdk::PlaybackHostView playback);
  // pj.mcp_endpoint.v1, optional: unbound makes every turn fail with a message saying so.
  void setMcpEndpointHost(PJ::sdk::McpEndpointHostView host);
  // pj.feature_catalog.v1, optional: an unbound view makes every turn carry the built-in
  // reference (fallback_features.hpp). With no provider at all, no features block is sent.
  void setFeatureCatalogProvider(std::function<PJ::sdk::FeatureCatalogView()> provider);
  void setSettings(PJ::sdk::SettingsView settings);

 private:
  friend class AssistantDialogTestPeer;

  void workerLoop();
  void postCommand(std::function<void()> fn);
  void postEvent(std::function<void()> fn);

  // GUI thread: appends input_text as a user turn, flips to WaitingForLlm and enqueues the
  // backend call on the worker thread.
  void sendCurrentInput();

  // GUI thread: this turn's "PlotJuggler features" block (TurnTools::features); empty when no
  // provider is bound.
  [[nodiscard]] std::string featuresForTurn();

  // Runs on the GUI thread (posted by the worker's sink through the event queue).
  void applyBackendEvent(const BackendEvent& ev);

  void commitSettings();
  // Blank/whitespace-only text removes the custom name; the row falls back to the harness's own
  // title, as renames elsewhere in PlotJuggler do.
  void commitRename();
  // Selects the backend implementation from settings. Returns whether active_backend_key_
  // changed, the signal both callers (commitSettings(), onTick's deferred branch) use to also
  // call activateBackendConversation(), since a backend switch is a conversation switch. Returns
  // false without touching anything when the rebuild must DEFER because a turn is in flight
  // (DialogState::rebuild_pending); onTick re-invokes it once the turn ends and acts on that
  // return value.
  bool rebuildBackend();

  // Drops the transcript, the accumulated cost, the ACTIVE backend's conversation memory
  // (memoryFor(active_backend_key_)) and its persisted session id, so a later reopen does not
  // resume what the user walked away from. No backend rebuild: the memory is cleared in place
  // and the live backend reads through the same pointer, so the next turn sends no --resume and
  // re-sends the catalog, with the MCP endpoint kept. Deletes nothing on disk (the old
  // conversation stays in the harness's store and the drawer; see onItemDeleteRequested). The
  // OTHER backend's memory and id are untouched.
  void startNewConversation();

  // Loads the transcript for `id` from the ACTIVE backend's store and replays it into a cleared
  // ChatSession through the same calls as the live path (appendAssistant merges consecutive
  // assistant text blocks into one row). Points memoryFor(active_backend_key_) at `id` with
  // resumed_pending set (composePayload re-sends the catalog with the resume note next turn) and
  // persists the id under that backend's key. Returns false, changing nothing, if the backend has
  // nothing under `id` (purged between listing and picking).
  bool switchToConversation(const std::string& id);

  // The conversation memory for one backend key ("claude"/"codex"/"echo"), created on first
  // request and kept for the dialog's life, so switching backends never restarts a chat.
  // GUI-thread only.
  std::shared_ptr<HarnessMemory> memoryFor(const std::string& key);

  // Restores the active conversation once, when the settings view is first bound (the ctor runs
  // against an unbound view). A persisted id that no longer resolves is cleared rather than left
  // as a dangling --resume target.
  void loadPersistedConversation();

  // Loads what the NOW-active backend (active_backend_key_, set by the preceding
  // rebuildBackend()) has persisted as its active conversation, or resets to a blank one if none
  // (or purged). Runs on first bind and whenever Settings swaps the backend key (commitSettings,
  // onTick's deferred rebuild), since each backend key keeps its OWN resume point. Always
  // refreshes the drawer listing and marks transcript/controls dirty so the panel matches what is
  // active. GUI-thread only; takes state_.mu itself, so must not be called with it held.
  void activateBackendConversation();

  // Requires state_.mu held. Rebuilds `conversations` from backend_->listConversations() (local
  // disk I/O, cheap enough inline on the GUI thread) and prunes conversation_titles to the ids
  // that listing still has, so the drawer never shows a name for a purged conversation.
  void refreshConversationsLocked();
  // Requires state_.mu held. The host's QListWidget protocol selects and reports rows by TEXT
  // (setSelectedItems / onSelectionChanged), so these map row text to conversation id and back;
  // O(n) over a human-sized list.
  [[nodiscard]] std::string idForListTextLocked(const std::string& text) const;
  [[nodiscard]] std::string listTextForIdLocked(const std::string& id) const;

  DialogState state_;
  // Acquired on the GUI thread every turn; the host hands back the same endpoint while held and
  // releases it when the plugin instance is destroyed.
  PJ::sdk::McpEndpointHostView mcp_host_;
  // shared_ptr: the worker captures its own reference for a turn, so a Settings commit swapping
  // this member mid-turn does not destroy the object under the worker.
  std::shared_ptr<LlmBackend> backend_;
  // Which key backend_ is ("claude"/"codex"/"echo" for an unknown persisted choice), set by
  // rebuildBackend(); selects the memories_ entry and `assistant.conv.<key>.session_id`.
  std::string active_backend_key_ = "claude";
  // One conversation memory PER backend key, outliving every backend object built for it, so each
  // backend resumes its own last conversation.
  std::map<std::string, std::shared_ptr<HarnessMemory>> memories_;

  std::thread worker_thread_;
  std::mutex cmd_mu_;
  std::condition_variable cmd_cv_;
  std::deque<std::function<void()>> cmd_queue_;
  bool worker_stop_ = false;

  std::mutex evt_mu_;
  std::deque<std::function<void()>> evt_queue_;

  PJ::sdk::ToolboxHostView toolbox_host_;
  PJ::sdk::PlaybackHostView playback_host_;
  std::function<PJ::sdk::FeatureCatalogView()> feature_catalog_provider_;
  PJ::sdk::SettingsView settings_;
  // One restore per instance: a re-bound setSettings would otherwise duplicate the transcript.
  bool conversation_loaded_ = false;
};

}  // namespace assistant_agent
