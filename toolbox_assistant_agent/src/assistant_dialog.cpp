// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "assistant_dialog.hpp"

#include <cstdlib>
#include <utility>

#include "assistant_panel_manifest.hpp"
#include "assistant_panel_ui.hpp"
#include "assistant_settings_ui.hpp"
#include "claude_backend.hpp"
#include "conversation_state.hpp"
#include "fake_backend.hpp"
#include "settings_store.hpp"

namespace assistant_agent {

namespace {
// Persisted settings keys (pj.settings.v1). Namespaced so they never collide
// with another toolbox's keys in the shared store.
constexpr const char* kKeyBackend = "assistant.backend";  // "claude" (harness backends join here)
constexpr const char* kKeyClaudeModel = "assistant.claude.model";
constexpr const char* kKeyClaudeCli = "assistant.claude.cli_path";

// Leaving this blank would pass no --model and get the CLI's own default, which
// is the slowest tier for no measurable gain: `sonnet` matches the fastest tier
// on turn time (10.1 s vs 10.4 s median) and was the only tier with no miss in
// the 240-cell study (docs/BENCHMARKS.md). A user who deliberately clears the
// field still gets the CLI default, because an empty stored value is returned
// as-is rather than falling back to this.
constexpr const char* kDefaultClaudeModel = "sonnet";

// A drawer row's text. The date suffix is what keeps two same-titled
// conversations apart under the host's text-keyed list protocol
// (setSelectedItems / onSelectionChanged match by TEXT, not by row or id).
std::string listText(const ConversationSummary& c) {
  return c.title + " · " + formatShortDate(c.last_ts);
}
}  // namespace

AssistantDialog::AssistantDialog() : claude_memory_(std::make_shared<ClaudeMemory>()) {
  rebuildBackend();
  worker_thread_ = std::thread([this]() { workerLoop(); });
}

AssistantDialog::~AssistantDialog() {
  // Ask the backend to abort any in-flight turn first — the worker may be
  // blocked inside a long HTTP post or subprocess wait, and join() below
  // would otherwise stall teardown until it finishes on its own.
  if (backend_) {
    backend_->cancel();
  }
  // Unblock any tool call waiting on the GUI thread, so the worker can
  // finish its turn and the join below doesn't deadlock.
  gui_executor_.shutdown();
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    worker_stop_ = true;
  }
  cmd_cv_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void AssistantDialog::rebuildBackend() {
  // Never mid-turn: the incoming and outgoing backends share the conversation
  // memory, and the worker may be writing a session id into it right now. The
  // Settings button is already disabled while busy, so in practice this only
  // catches a host-driven setSettings() landing during a turn.
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.session.busy()) {
      state_.rebuild_pending = true;
      return;
    }
    state_.rebuild_pending = false;
  }

  // The scripted FakeBackend is an explicit opt-in for driving the tool path
  // without an LLM (unit tests + manual E2E). Otherwise the persisted
  // 'assistant.backend' choice selects the backend. Claude is the only one
  // until the harness backends land (docs/NORTH_STAR.md), so an absent choice
  // gets it; a choice this build has never heard of falls back to the harmless
  // echo backend, visibly labeled. (A store still saying "ollama" was migrated
  // when the settings view was bound — see setSettings.)
  const char* fake = std::getenv("ASSISTANT_FAKE_BACKEND");
  if (fake != nullptr && std::string(fake) == "1") {
    backend_ = std::make_shared<FakeBackend>();
  } else {
    SettingsStore store(settings_);
    if (store.getString(kKeyBackend, "claude") == "claude") {
      backend_ = std::make_shared<ClaudeBackend>(
          store.getString(kKeyClaudeCli, "claude"), store.getString(kKeyClaudeModel, kDefaultClaudeModel),
          claude_memory_);
    } else {
      backend_ = std::make_shared<EchoBackend>();
    }
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.backend_name = backend_->name();
  state_.controls_dirty = true;  // the status line names the backend
}

void AssistantDialog::startNewConversation() {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (state_.session.busy()) {
    return;  // the button is disabled while busy; this is the belt to that brace
  }
  state_.session.clear();
  state_.usage.reset();
  // Clearing in place is what makes this work for whichever backend is live:
  // it reads its memory through the pointer the dialog still holds.
  *claude_memory_ = ClaudeMemory{};
  state_.active_conversation_id.clear();
  // The persisted pointer goes too: New chat is the one gesture that frees
  // the user from the past, and a reopen must not resume it. Nothing is
  // deleted on disk — the old conversation stays in the harness's store and
  // the drawer, just no longer active.
  SettingsStore store(settings_);
  clearActiveSessionId(store);
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
  state_.drawer_dirty = true;  // the active row changes; the drawer itself stays as it is
}

bool AssistantDialog::switchToConversation(const std::string& id) {
  const std::vector<ChatMessage> transcript = backend_ ? backend_->loadTranscript(id) : std::vector<ChatMessage>{};
  if (transcript.empty()) {
    return false;  // nothing under this id -- purged, or a stale drawer row
  }

  std::lock_guard<std::mutex> lock(state_.mu);
  state_.session.clear();
  state_.usage.reset();
  for (const ChatMessage& m : transcript) {
    switch (m.role) {
      case ChatMessage::Role::User:
        state_.session.addUser(m.text);
        break;
      case ChatMessage::Role::Assistant:
        // appendAssistant, not addAssistant: merges consecutive blocks into
        // one row exactly like a live streamed reply does, so this renders
        // identically to how the turn looked when it happened.
        state_.session.appendAssistant(m.text);
        break;
      case ChatMessage::Role::Tool:
        state_.session.addTool(m.text);
        break;
      case ChatMessage::Role::System:
        state_.session.addSystem(m.text);
        break;
    }
  }
  // Visible seam between then and now — the model's memory comes back through
  // --resume regardless; this keeps the user able to SEE that it did.
  state_.session.addSystem("resumed previous conversation");

  *claude_memory_ = ClaudeMemory{};
  claude_memory_->session_id = id;
  // Forces composePayload to re-send the catalog with the resume note on the
  // next turn: this process's ephemeral state (tabs the model composed, etc.)
  // died with whatever process wrote this transcript, and --resume would
  // otherwise replay history as if it hadn't.
  claude_memory_->resumed_pending = true;

  state_.active_conversation_id = id;
  SettingsStore store(settings_);
  saveActiveSessionId(store, id);

  // The drawer stays open: it is a sidebar the user browses, not a menu that
  // dismisses itself once picked from.
  state_.drawer_dirty = true;
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
  return true;
}

void AssistantDialog::loadPersistedConversation() {
  SettingsStore store(settings_);
  const std::string id = loadActiveSessionId(store);
  if (id.empty()) {
    return;
  }
  if (!switchToConversation(id)) {
    // Purged between the last close and this reopen: nothing to resume into.
    clearActiveSessionId(store);
  }
}

void AssistantDialog::refreshConversationsLocked() {
  state_.conversations = backend_ ? backend_->listConversations() : std::vector<ConversationSummary>{};
}

std::string AssistantDialog::idForListTextLocked(const std::string& text) const {
  for (const ConversationSummary& c : state_.conversations) {
    if (listText(c) == text) {
      return c.id;
    }
  }
  return {};
}

std::string AssistantDialog::listTextForIdLocked(const std::string& id) const {
  for (const ConversationSummary& c : state_.conversations) {
    if (c.id == id) {
      return listText(c);
    }
  }
  return {};
}

std::string AssistantDialog::manifest() const {
  return kAssistantPanelManifest;
}

std::string AssistantDialog::ui_content() const {
  return kAssistantPanelUi;
}

void AssistantDialog::setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider) {
  host_provider_ = std::move(provider);
}

void AssistantDialog::setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider) {
  runtime_host_provider_ = std::move(provider);
}

void AssistantDialog::setDataProcessorsProvider(std::function<PJ::sdk::DataProcessorsHostView()> provider) {
  dp_provider_ = std::move(provider);
}

void AssistantDialog::setObjectReadProvider(std::function<PJ::sdk::ToolboxObjectReadHostView()> provider) {
  object_read_provider_ = std::move(provider);
}

void AssistantDialog::setPlaybackProvider(std::function<PJ::sdk::PlaybackHostView()> provider) {
  playback_provider_ = std::move(provider);
}

void AssistantDialog::setViewportProvider(std::function<PJ::sdk::ViewportHostView()> provider) {
  viewport_provider_ = std::move(provider);
}

void AssistantDialog::setPlotTabsProvider(std::function<PJ::sdk::PlotTabHostView()> provider) {
  plot_tabs_provider_ = std::move(provider);
}

void AssistantDialog::setSettings(PJ::sdk::SettingsView settings) {
  settings_ = settings;
  if (!conversation_loaded_) {
    conversation_loaded_ = true;
    // First bind is the one moment the store is both readable and fresh:
    // migrate an Ollama-era store before anything reads the backend choice.
    SettingsStore store(settings_);
    if (store.getString(kKeyBackend, "") == "ollama") {
      store.setString(kKeyBackend, "claude");
    }
    scrubRetiredOllamaKeys(store);
    scrubLegacyConversationKeys(store);
    loadPersistedConversation();
  }
  rebuildBackend();
}

ToolContext AssistantDialog::makeToolContext() {
  ToolContext ctx;
  if (host_provider_) {
    ctx.host = host_provider_();
  }
  if (dp_provider_) {
    ctx.dp = dp_provider_();
  }
  if (object_read_provider_) {
    ctx.objects = object_read_provider_();
  }
  if (playback_provider_) {
    ctx.playback = playback_provider_();
  }
  if (viewport_provider_) {
    ctx.viewport = viewport_provider_();
  }
  if (plot_tabs_provider_) {
    ctx.plot_tabs = plot_tabs_provider_();
  }
  if (runtime_host_provider_) {
    const PJ::ToolboxRuntimeHostView rt = runtime_host_provider_();
    ctx.notify_data_changed = [rt]() { rt.notifyDataChanged(); };
  }
  return ctx;
}

std::string AssistantDialog::widget_data() {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Steady state of an open panel: nothing changed, so skip building +
  // serializing a WidgetData 20x/sec — the host treats an empty return as
  // "no update".
  if (!state_.transcript_dirty && !state_.controls_dirty && !state_.clear_input_pending &&
      !state_.open_settings_pending && !state_.drawer_dirty && !state_.header_icons_pending) {
    return {};
  }
  PJ::WidgetData wd;

  if (state_.transcript_dirty) {
    wd.setPlainText("transcriptText", state_.session.render());
    state_.transcript_dirty = false;
  }

  if (state_.controls_dirty) {
    const bool busy = state_.session.busy();
    // The turn state, which backend answers, then what the last turn moved
    // (summary() is "" when there is nothing to report): one line of status,
    // since the header row above it is host chrome now.
    wd.setLabel(
        "statusLabel", state_.session.statusText() + "  \xc2\xb7  " + state_.backend_name + state_.usage.summary());
    wd.setEnabled("inputEdit", !busy);
    wd.setEnabled("sendButton", !busy);
    wd.setEnabled("cancelButton", busy);
    // A mid-turn backend rebuild would strand the in-flight turn on the old
    // backend (kept alive by the worker's reference) — just don't offer it.
    wd.setEnabled("settingsButton", !busy);
    // Same reason, plus: wiping the conversation memory under a running turn
    // would have the backend resume a session it just forgot.
    wd.setEnabled("newChatButton", !busy);
    // The drawer reads/mutates the same conversation memory a running turn is
    // writing into — same reason, same guard.
    wd.setEnabled("menuButton", !busy);
    wd.setEnabled("conversationList", !busy);
    state_.controls_dirty = false;
  }

  if (state_.header_icons_pending) {
    // Static per-panel glyphs; no reason to resend on every build like the
    // per-turn state above.
    wd.setButtonIconNamed("menuButton", "menu");
    wd.setButtonIconNamed("newChatButton", "add");
    state_.header_icons_pending = false;
  }

  if (state_.drawer_dirty) {
    wd.setVisible("conversationsDrawer", state_.drawer_open);
    std::vector<std::string> rows;
    rows.reserve(state_.conversations.size());
    for (const ConversationSummary& c : state_.conversations) {
      rows.push_back(listText(c));
    }
    wd.setListItems("conversationList", rows);
    wd.setListItemsDeletable("conversationList", true);
    wd.setListPlaceholder("conversationList", "No conversations yet");
    const std::string active_text = listTextForIdLocked(state_.active_conversation_id);
    wd.setSelectedItems(
        "conversationList", active_text.empty() ? std::vector<std::string>{} : std::vector<std::string>{active_text});
    state_.drawer_dirty = false;
  }

  if (state_.clear_input_pending) {
    wd.setText("inputEdit", "");
    state_.clear_input_pending = false;
  }

  if (state_.open_settings_pending) {
    state_.open_settings_pending = false;
    // Pre-fill the modal from persisted settings before showing it.
    SettingsStore store(settings_);
    wd.setCurrentIndex("backendCombo", 0);
    wd.setText("claudeModelEdit", store.getString(kKeyClaudeModel, kDefaultClaudeModel));
    wd.setText("claudeCliPathEdit", store.getString(kKeyClaudeCli, "claude"));
    wd.requestSubDialog(kAssistantSettingsUi);
  }

  return wd.toJson();
}

bool AssistantDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "inputEdit") {
    state_.input_text = std::string(text);
    return false;  // no re-render; the widget already shows the text
  }
  // Settings sub-dialog inputs: stage the value; commit on subDialogAccepted.
  if (widget_name == "claudeModelEdit") {
    state_.pending_claude_model = std::string(text);
    return false;
  }
  if (widget_name == "claudeCliPathEdit") {
    state_.pending_claude_cli = std::string(text);
    return false;
  }
  return false;
}

bool AssistantDialog::onIndexChanged(std::string_view widget_name, int index) {
  // No combo drives state today: backendCombo has one entry until the harness
  // backends land, and the backend key is normalized at bind time instead.
  (void)widget_name;
  (void)index;
  return false;
}

bool AssistantDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "sendButton") {
    sendCurrentInput();
    return true;
  }
  if (widget_name == "cancelButton") {
    if (backend_) {
      backend_->cancel();
    }
    return false;
  }
  if (widget_name == "settingsButton") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.open_settings_pending = true;
    return true;
  }
  if (widget_name == "newChatButton") {
    startNewConversation();
    return true;
  }
  if (widget_name == "menuButton") {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.drawer_open = !state_.drawer_open;
    if (state_.drawer_open) {
      refreshConversationsLocked();
    }
    state_.drawer_dirty = true;
    return true;
  }
  if (widget_name == "subDialogAccepted") {
    commitSettings();
    return true;
  }
  return false;
}

bool AssistantDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "conversationList") {
    return false;
  }
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (selected.empty() || state_.session.busy()) {
      return false;
    }
    id = idForListTextLocked(selected.front());
    if (id.empty() || id == state_.active_conversation_id) {
      return false;  // re-selecting the active row, or a row that raced with a delete
    }
  }
  // Unlocked: switchToConversation takes state_.mu itself (non-recursive).
  switchToConversation(id);
  return true;
}

bool AssistantDialog::onItemDeleteRequested(std::string_view widget_name, int index) {
  if (widget_name != "conversationList") {
    return false;
  }
  std::string id;
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.session.busy()) {
      return false;  // the list is disabled while busy; this is the belt to that brace
    }
    if (index < 0 || static_cast<std::size_t>(index) >= state_.conversations.size()) {
      return false;
    }
    id = state_.conversations[static_cast<std::size_t>(index)].id;
    was_active = (id == state_.active_conversation_id);
  }
  if (backend_) {
    backend_->deleteConversation(id);
  }
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    refreshConversationsLocked();
    state_.drawer_dirty = true;
  }
  if (was_active) {
    // New chat, not a blank dead-end: startNewConversation also clears the
    // active id and the in-memory session/memory.
    startNewConversation();
  }
  return true;
}

bool AssistantDialog::onTick() {
  // Two GUI-thread drains per tick:
  //  1. Tool calls the worker submitted — execute them here (host services are
  //     only legal on this thread) and unblock the waiting worker.
  //  2. Worker-posted event closures — each mutates state_ and sets dirty flags.
  // Both must run on this thread; ordering tools first lets a just-unblocked
  // worker's follow-up events be picked up as early as the next tick.
  std::size_t tools_ran = 0;
  if (!gui_executor_.empty()) {
    // Tool calls are rare (user-driven); don't pay the provider calls + the
    // notify closure allocation on every idle 20 Hz tick.
    ToolContext ctx = makeToolContext();
    tools_ran = gui_executor_.drain(registry_, ctx);
  }

  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(evt_mu_);
    batch.swap(evt_queue_);
  }
  for (auto& fn : batch) {
    fn();
  }

  // A backend rebuild that had to wait for the turn to finish. The flag is read
  // under the lock, but rebuildBackend() is called outside it: it takes the same
  // non-recursive mutex, and it re-checks busy() and clears the flag itself.
  bool rebuild = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    rebuild = state_.rebuild_pending && !state_.session.busy();
  }
  if (rebuild) {
    rebuildBackend();
  }

  return tools_ran > 0 || !batch.empty() || rebuild;
}

void AssistantDialog::sendCurrentInput() {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.session.busy()) {
      return;  // ignore Send while a turn is in flight
    }
    text = state_.input_text;
    if (text.empty()) {
      return;
    }
    state_.input_text.clear();
    state_.clear_input_pending = true;
    state_.session.addUser(text);
    state_.session.setState(TurnState::WaitingForLlm);
    state_.transcript_dirty = true;
    state_.controls_dirty = true;
  }

  // Hand the turn to the worker thread. The sink marshals every backend event
  // back to the GUI thread via the event queue; tools.invoke marshals tool
  // calls to the GUI thread via the GuiExecutor (blocking the worker until
  // onTick runs them). The lambda captures its OWN backend reference (still on
  // the GUI thread here): a mid-turn Settings commit may swap backend_, and the
  // captured reference keeps the in-flight object alive for the whole turn.
  // Snapshot what is loaded here, on the GUI thread, where the host view is
  // legal to touch — the worker cannot call host services itself. Rebuilt every
  // turn rather than cached, so that loading another file or starting a stream
  // is reflected; it is host metadata, so the scan is cheap. The backend
  // decides whether this turn actually needs to carry it.
  std::string catalog;
  if (host_provider_) {
    catalog = catalogDigest(host_provider_());
  }

  postCommand([this, text, catalog, backend = backend_]() {
    LlmBackend::EventSink sink = [this](BackendEvent ev) { postEvent([this, ev]() { applyBackendEvent(ev); }); };
    TurnTools tools;
    tools.registry = &registry_;
    tools.catalog = catalog;
    tools.invoke = [this](const std::string& name, const nlohmann::json& args) {
      return gui_executor_.call(name, args);
    };
    if (backend) {
      backend->sendUserMessage(text, tools, sink);
    } else {
      postEvent([this]() {
        applyBackendEvent({BackendEvent::Kind::Error, "No backend configured."});
        applyBackendEvent({BackendEvent::Kind::TurnComplete, {}});
      });
    }
  });
}

void AssistantDialog::applyBackendEvent(const BackendEvent& ev) {
  std::lock_guard<std::mutex> lock(state_.mu);
  switch (ev.kind) {
    case BackendEvent::Kind::AssistantText:
      // Append, don't add: a streaming backend sends one reply as many chunks.
      state_.session.appendAssistant(ev.text);
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::ToolActivity:
      state_.session.addTool(ev.text);
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::Error:
      state_.session.addSystem("Error: " + ev.text);
      if (ev.resume_failed) {
        // The backend established that --resume itself failed (the session
        // was purged between listing it and this turn): say the one thing the
        // error text does not — retrying will not help. The transcript stays
        // readable; New chat is the way forward.
        state_.session.addSystem("This conversation can no longer be continued; start a new one.");
      }
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::Metrics:
      // No repaint of its own: TurnComplete follows immediately and its
      // controls_dirty is what re-renders the status label.
      state_.usage.record(ev.metrics);
      break;
    case BackendEvent::Kind::TurnComplete:
      state_.session.setState(TurnState::Idle);
      state_.controls_dirty = true;
      // Persist the active session id once it actually changes — a brand-new
      // conversation gets its id here for the first time; a resumed one
      // already has it saved, so this is then a no-op read.
      if (!claude_memory_->session_id.empty() && claude_memory_->session_id != state_.active_conversation_id) {
        state_.active_conversation_id = claude_memory_->session_id;
        SettingsStore store(settings_);
        saveActiveSessionId(store, state_.active_conversation_id);
      }
      break;
  }
}

void AssistantDialog::commitSettings() {
  SettingsStore store(settings_);
  std::string backend;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    // Persist each staged edit and clear it for the next time the modal opens.
    const std::pair<const char*, std::optional<std::string>*> staged[] = {
        {kKeyClaudeModel, &state_.pending_claude_model},
        {kKeyClaudeCli, &state_.pending_claude_cli},
    };
    for (const auto& [key, value] : staged) {
      if (value->has_value()) {
        store.setString(key, **value);
        value->reset();
      }
    }
    backend = store.getString(kKeyBackend, "claude");

    state_.session.addSystem("Settings saved (backend: " + backend + ").");
    state_.transcript_dirty = true;
  }
  // Re-derive the header label + active backend from the freshly persisted choice.
  rebuildBackend();

  // Probe the new backend off the GUI thread and report the result. Capture a
  // reference (still on the GUI thread here) so a subsequent rebuild can't
  // swap the object out from under the probe.
  postCommand([this, backend = backend_]() {
    BackendTestResult t = backend ? backend->testConnection() : BackendTestResult{false, "no backend"};
    postEvent([this, t]() {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.session.addSystem(std::string("Connection: ") + (t.ok ? "OK - " : "FAILED - ") + t.message);
      state_.transcript_dirty = true;
    });
  });
}

void AssistantDialog::workerLoop() {
  for (;;) {
    std::function<void()> cmd;
    {
      std::unique_lock<std::mutex> lock(cmd_mu_);
      cmd_cv_.wait(lock, [this]() { return worker_stop_ || !cmd_queue_.empty(); });
      if (worker_stop_ && cmd_queue_.empty()) {
        return;
      }
      cmd = std::move(cmd_queue_.front());
      cmd_queue_.pop_front();
    }
    cmd();
  }
}

void AssistantDialog::postCommand(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    cmd_queue_.push_back(std::move(fn));
  }
  cmd_cv_.notify_one();
}

void AssistantDialog::postEvent(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(evt_mu_);
  evt_queue_.push_back(std::move(fn));
}

}  // namespace assistant_agent
