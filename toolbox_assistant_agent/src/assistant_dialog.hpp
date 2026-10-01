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
#include "gui_executor.hpp"
#include "llm_backend.hpp"
#include "tool_registry.hpp"
#include "usage_ledger.hpp"

namespace assistant_agent {

class AssistantDialogTestPeer;
class SettingsStore;

// Character budget for the catalog digest handed to the model at the top of
// every turn (catalogDigest, tool_registry.hpp), read from the settings key
// "assistant.catalog_budget_chars" and clamped to [1000, 200000]. An absent
// or unparsable value falls back to catalogDigest's own default (10000) via
// SettingsStore::getInt, so an install that never touched the key sees
// byte-identical output. Declared here (rather than kept anonymous in the
// .cpp, like resolveBackendKey) so tests can drive it without wiring a full
// AssistantDialog + host; sendCurrentInput() calls it fresh every turn (no
// caching), so a value written to the conf before the app starts takes effect.
[[nodiscard]] int resolveCatalogBudgetChars(const SettingsStore& store);

// Per-conversation state a backend borrows (harness_memory.hpp).
// Forward-declared so this header does not pull the backend implementations —
// and their JSON dependency — into everything that includes it; the dialog's
// destructor lives in the .cpp, where it is complete.
struct HarnessMemory;

// Which sub-dialog PanelEngine's single synthetic "subDialogAccepted" click
// (fired for ANY accepted sub-dialog, host side: panel_engine.cpp) refers to.
// Set when a sub-dialog is requested (widget_data()'s two *_pending blocks),
// routed and cleared at accept (onClicked), and also cleared the next time
// widget_data() runs with nothing newly pending -- the backstop for a Cancel,
// which never fires subDialogAccepted and would otherwise leave this pointing
// at a dialog that is no longer on screen.
enum class SubDialogKind { None, Settings, Rename };

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
  // re-push (setPlainText on a growing transcript) is the expensive one, so
  // it is gated behind an explicit change signal (risk R5).
  bool transcript_dirty = true;
  bool controls_dirty = true;
  // Clear the input box exactly once, right after a Send (never mid-typing).
  bool clear_input_pending = false;

  // What the last turn moved; rendered after statusText().
  UsageLedger usage;

  // The conversations drawer, always visible (left of the transcript).
  // Populated from backend_->listConversations() on bind, on a backend switch,
  // after a delete, and after every completed turn.
  bool drawer_dirty = true;          // forces the first widget_data() to push the list + placeholder
  bool header_icons_pending = true;  // one-shot setButtonIconNamed for newChatButton
  std::vector<ConversationSummary> conversations;
  std::string active_conversation_id;
  // Custom display names for `conversations`, keyed by conversation id --
  // refreshed alongside `conversations` itself (refreshConversationsLocked),
  // after the harness's own titles have already been pruned of anything that
  // fell out of that same listing. See conversation_state.hpp.
  ConversationTitles conversation_titles;

  // A backend rebuild that arrived mid-turn and has to wait. Swapping the
  // backend is safe (the worker holds its own reference), but the conversation
  // memory is now SHARED between the outgoing and incoming objects, so doing it
  // while the worker is writing a session id is a data race. Applied by onTick
  // once the turn is over — outside the state lock, because rebuildBackend()
  // takes it and the mutex is not recursive.
  bool rebuild_pending = false;

  // Which sub-dialog widget_data() last requested — see SubDialogKind's own
  // comment for why this has to exist at all (one synthetic accept click,
  // two possible dialogs).
  SubDialogKind open_sub_dialog = SubDialogKind::None;

  // Settings sub-dialog: request flag (read+cleared in widget_data) plus the
  // staged edits harvested from the modal's inputs on OK. PanelEngine fires
  // onTextChanged/onIndexChanged for each child, then onClicked("subDialogAccepted")
  // to commit — mirroring the toolbox_mosaico cert-dialog handshake.
  bool open_settings_pending = false;
  // Staged text edits, keyed by SETTINGS key (not widget name) — onTextChanged
  // resolves the widget name to a settings key via each BackendSpec's key
  // (widgetName(), assistant_dialog.cpp), so this holds e.g.
  // "assistant.claude.model" -> the typed value. commitSettings persists
  // every entry and clears the map back to empty.
  std::map<std::string, std::string> pending_text;
  // Staged backendCombo choice ("claude"/"codex"); onIndexChanged sets it,
  // commitSettings persists it and clears it back to nullopt.
  std::optional<std::string> pending_backend;

  // Rename sub-dialog: same request/stage/commit shape as Settings above, one
  // field each because there is exactly one input (ui/rename_conversation.ui).
  bool open_rename_pending = false;
  // The conversation the context-menu action was fired on (resolved from the
  // row index through `conversations` at request time, onItemContextAction) —
  // NOT necessarily active_conversation_id; renaming never changes which
  // conversation is open.
  std::string rename_conversation_id;
  // renameEdit's prefill (the row's current display name), sent once when the
  // sub-dialog is requested.
  std::string rename_prefill;
  // renameEdit's live text, staged by onTextChanged exactly like a settings
  // field and committed on accept.
  std::optional<std::string> pending_rename_text;
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
  bool onItemContextAction(std::string_view widget_name, int index, std::string_view action_id) override;
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
  friend class AssistantDialogTestPeer;

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
  // Persist (or clear) the staged rename committed by the rename sub-dialog.
  // Blank/whitespace-only text removes the custom name instead of storing it
  // — the row falls back to the harness's own title, the house behavior for
  // renames elsewhere in PlotJuggler.
  void commitRename();
  // Build a ToolContext from the currently-bound host providers (GUI thread).
  ToolContext makeToolContext();
  // Select the backend implementation from settings + the ASSISTANT_FAKE_BACKEND
  // env override. Rebuilt whenever the backend choice changes. Returns whether
  // active_backend_key_ actually changed as a result of this call -- the
  // signal both call sites (commitSettings(), onTick's deferred branch) use to
  // decide whether to also call activateBackendConversation(), since a
  // backend switch is a conversation switch. Also false, without touching
  // anything, when the rebuild has to DEFER itself because a turn is in
  // flight (DialogState::rebuild_pending; the incoming and outgoing backends
  // share the conversation memory, and the worker may be writing a session id
  // into it right now) -- onTick re-invokes this once the turn ends and reads
  // ITS return value then, which is what makes "if (rebuildBackend())
  // activateBackendConversation();" the whole story at both call sites.
  bool rebuildBackend();

  // Drop the transcript, the accumulated cost and the ACTIVE backend's
  // conversation memory (memoryFor(active_backend_key_)) — AND that backend's
  // active-session id in the settings store, so a later reopen does not
  // resume what the user explicitly walked away from. No backend rebuild: the
  // memory is cleared in place and the live backend reads through the same
  // pointer, so the next turn sends no --resume and re-sends the catalog —
  // genuinely fresh, with the MCP server left up. This does NOT delete
  // anything on disk: the old conversation stays in the harness's store and
  // in the drawer, just no longer active (New chat is not the trash can —
  // see onItemDeleteRequested for that). The OTHER backend's memory (and
  // persisted id) is untouched — switching backends must never look like
  // "New chat" to the one not currently active.
  void startNewConversation();

  // Load the transcript for `id` from the ACTIVE backend's own store and
  // replay it into a cleared ChatSession via the SAME calls the live path
  // uses (addUser/appendAssistant/addTool), so a resumed transcript renders
  // identically to one built turn-by-turn — appendAssistant is what merges
  // consecutive assistant text blocks into one row. Points
  // memoryFor(active_backend_key_) at `id` with resumed_pending set
  // (composePayload then re-sends the catalog with the resume note on the
  // next turn) and persists the new active id under that backend's key. The
  // drawer stays as it was — a sidebar, not a menu. Returns false without
  // changing anything if the backend has nothing under `id` (purged between
  // listing and picking it).
  bool switchToConversation(const std::string& id);

  // The shared conversation memory for one backend key ("claude"/"codex"/
  // "echo"), created on first request and reused for the life of the dialog —
  // this is what makes switching backends never silently restart a chat: each
  // key's HarnessMemory just sits there, untouched, while another key's
  // backend is the one live. GUI-thread only, like backend_ itself.
  std::shared_ptr<HarnessMemory> memoryFor(const std::string& key);

  // Restore the active conversation once when the settings view is first
  // bound (the ctor runs before bind(), against an unbound view, so this
  // cannot run there). If the persisted id no longer resolves to anything
  // (purged since the last close), clears it instead of leaving a dangling
  // --resume target. A thin wrapper over activateBackendConversation() now
  // that a settings-driven backend switch needs the exact same logic.
  void loadPersistedConversation();

  // Loads whatever the NOW-active backend (active_backend_key_, already set
  // by the rebuildBackend() call the caller just made) has persisted as its
  // active conversation -- or, if nothing is persisted (or it was purged),
  // resets to a blank one. Used on the very first bind (via
  // loadPersistedConversation) AND every time Settings actually swaps the
  // backend key (commitSettings, and onTick's deferred-rebuild path): a
  // backend switch is a conversation switch, because each backend key keeps
  // its OWN resume point (memoryFor). Always refreshes the drawer's listing
  // (and marks it dirty): the drawer has no closed state to defer the work to
  // anymore, and the panel a settings commit leaves behind must always match
  // what is now active. transcript_dirty/controls_dirty are marked
  // unconditionally for the same reason, whichever branch ran. GUI-thread
  // only; takes state_.mu itself (like switchToConversation), so must not be
  // called with it already held.
  void activateBackendConversation();

  // Requires state_.mu held by the caller. Rebuilds `conversations` from
  // backend_->listConversations() — local disk I/O under the harness's
  // project dir, cheap enough to run inline on the GUI thread on every call
  // site. Also prunes conversation_titles down to the ids this
  // listing still has (conversation_state.hpp) and reloads it, so the drawer
  // never shows a name for a conversation the harness has since purged.
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
  // Which key backend_ currently is ("claude"/"codex"/"echo" for an unknown
  // persisted choice) — set by rebuildBackend(), read by startNewConversation/
  // switchToConversation/the TurnComplete handler to know which memories_
  // entry and which `assistant.conv.<key>.session_id` to touch.
  std::string active_backend_key_ = "claude";
  // One conversation memory PER backend key, each outliving every backend
  // object built for that key. Populated lazily by memoryFor(); switching
  // backends (a settings change) never touches an entry it isn't currently
  // using, which is what lets each backend resume its own last conversation.
  std::map<std::string, std::shared_ptr<HarnessMemory>> memories_;

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
