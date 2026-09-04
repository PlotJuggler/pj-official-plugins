// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "assistant_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

#include "assistant_panel_manifest.hpp"
#include "assistant_panel_ui.hpp"
#include "assistant_settings_ui.hpp"
#include "claude_backend.hpp"
#include "codex_backend.hpp"
#include "conversation_state.hpp"
#include "fake_backend.hpp"
#include "model_picker.hpp"
#include "rename_conversation_ui.hpp"
#include "settings_store.hpp"

namespace assistant_agent {

namespace {
// Persisted settings keys (pj.settings.v1). Namespaced so they never collide
// with another toolbox's keys in the shared store.
constexpr const char* kKeyBackend = "assistant.backend";  // "claude" or "codex"

// One row per backend this build knows about: its settings key, the
// backendCombo index it fills, the settings keys/defaults for its
// model/cli-path fields, and two function pointers -- `make` builds the
// backend object (rebuildBackend()'s job) and `models` lists what it offers
// in the settings combo (the Settings code's job) -- so neither has to switch
// on `key` or spin up a throwaway instance to reach a virtual. Widget names
// are not stored here; see widgetName() below.
struct BackendSpec {
  const char* key;
  int combo_index;
  const char* model_key;
  const char* cli_key;
  const char* default_model;
  const char* default_cli;
  std::shared_ptr<LlmBackend> (*make)(std::string cli, std::string model, std::shared_ptr<HarnessMemory> memory);
  std::vector<ModelChoice> (*models)();
};

std::shared_ptr<LlmBackend> makeClaudeBackend(
    std::string cli, std::string model, std::shared_ptr<HarnessMemory> memory) {
  return std::make_shared<ClaudeBackend>(std::move(cli), std::move(model), std::move(memory));
}
std::shared_ptr<LlmBackend> makeCodexBackend(
    std::string cli, std::string model, std::shared_ptr<HarnessMemory> memory) {
  return std::make_shared<CodexBackend>(std::move(cli), std::move(model), std::move(memory));
}

constexpr BackendSpec kBackends[] = {
    // Leaving this blank would pass no --model and get the CLI's own default,
    // which is the slowest tier for no measurable gain: `sonnet` matches the
    // fastest tier on turn time (10.1 s vs 10.4 s median) and was the only
    // tier with no miss in the 240-cell study (docs/BENCHMARKS.md). A user who
    // deliberately clears the field still gets the CLI default, because an
    // empty stored value is returned as-is rather than falling back to this.
    {"claude", 0, "assistant.claude.model", "assistant.claude.cli_path", "sonnet", "claude", &makeClaudeBackend,
     &ClaudeBackend::listModels},
    // Codex has had no equivalent benchmark run yet, so an empty default just
    // means "whatever `codex exec` picks on its own" rather than a measured pick.
    {"codex", 1, "assistant.codex.model", "assistant.codex.cli_path", "", "codex", &makeCodexBackend,
     &CodexBackend::listModels},
};

// A BackendSpec field's widget name, e.g. widgetName(kBackends[0], "ModelCombo")
// == "claudeModelCombo" (ui/assistant_settings.ui). Derived from `key` instead
// of stored per-field: the field never has to be told twice which backend it
// belongs to.
std::string widgetName(const BackendSpec& spec, const char* suffix) {
  return std::string(spec.key) + suffix;
}

// The spec whose key/combo_index matches, or nullptr for a choice this build
// has never heard of.
const BackendSpec* backendByKey(std::string_view key) {
  for (const BackendSpec& spec : kBackends) {
    if (key == spec.key) {
      return &spec;
    }
  }
  return nullptr;
}

const BackendSpec* backendByIndex(int index) {
  for (const BackendSpec& spec : kBackends) {
    if (spec.combo_index == index) {
      return &spec;
    }
  }
  return nullptr;
}

// The name a row shows: the user's custom name (assistant.conv.<key>.titles,
// conversation_state.hpp) if one is set, else the harness's own title.
std::string displayTitle(const ConversationSummary& c, const ConversationTitles& titles) {
  const auto it = titles.find(c.id);
  return it != titles.end() ? it->second : c.title;
}

// A drawer row's text. The date suffix is what keeps two same-titled
// conversations apart under the host's text-keyed list protocol
// (setSelectedItems / onSelectionChanged match by TEXT, not by row or id).
std::string listText(const ConversationSummary& c, const ConversationTitles& titles) {
  return displayTitle(c, titles) + " · " + formatShortDate(c.last_ts);
}

// True for an empty or whitespace-only rename: the house behavior elsewhere
// in PlotJuggler is that clearing a name resets it, not that it stores "".
bool isBlank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}

// Which backend key a persisted `assistant.backend` value resolves to —
// shared by rebuildBackend() (which needs it to build the right object) and
// loadPersistedConversation() (which needs it BEFORE a backend exists, to
// know which `assistant.conv.<key>.session_id` to read). "echo" for a choice
// this build has never heard of: the harmless fallback backend has no store
// of its own, so its "memory" is simply never resumed into.
std::string resolveBackendKey(const SettingsStore& store) {
  const std::string choice = store.getString(kKeyBackend, "claude");
  return backendByKey(choice) ? choice : "echo";
}
}  // namespace

AssistantDialog::AssistantDialog() {
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

std::shared_ptr<HarnessMemory> AssistantDialog::memoryFor(const std::string& key) {
  auto it = memories_.find(key);
  if (it != memories_.end()) {
    return it->second;
  }
  auto memory = std::make_shared<HarnessMemory>();
  memories_.emplace(key, memory);
  return memory;
}

bool AssistantDialog::rebuildBackend() {
  // Never mid-turn: the incoming and outgoing backends share the conversation
  // memory, and the worker may be writing a session id into it right now. The
  // Settings button is already disabled while busy, so in practice this only
  // catches a host-driven setSettings() landing during a turn. Deferred here,
  // this reports no change (false); onTick applies the real rebuild once the
  // turn ends and acts on THAT call's return value instead.
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.session.busy()) {
      state_.rebuild_pending = true;
      return false;
    }
    state_.rebuild_pending = false;
  }

  const std::string previous_key = active_backend_key_;

  // The scripted FakeBackend is an explicit opt-in for driving the tool path
  // without an LLM (unit tests + manual E2E). Otherwise the persisted
  // 'assistant.backend' choice selects the backend: "claude" or "codex" each
  // get their own conversation memory (memoryFor), so switching between them
  // never touches the other's; a choice this build has never heard of falls
  // back to the harmless echo backend, visibly labeled. (A store still saying
  // "ollama" was migrated when the settings view was bound — see setSettings.)
  const char* fake = std::getenv("ASSISTANT_FAKE_BACKEND");
  if (fake != nullptr && std::string(fake) == "1") {
    backend_ = std::make_shared<FakeBackend>();
    active_backend_key_ = "echo";  // FakeBackend keeps no memory of its own
  } else {
    SettingsStore store(settings_);
    active_backend_key_ = resolveBackendKey(store);
    if (const BackendSpec* spec = backendByKey(active_backend_key_)) {
      backend_ = spec->make(
          store.getString(spec->cli_key, spec->default_cli), store.getString(spec->model_key, spec->default_model),
          memoryFor(spec->key));
    } else {
      backend_ = std::make_shared<EchoBackend>();
    }
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.backend_name = backend_->name();
  state_.controls_dirty = true;  // the status line names the backend
  return active_backend_key_ != previous_key;
}

void AssistantDialog::startNewConversation() {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (state_.session.busy()) {
    return;  // the button is disabled while busy; this is the belt to that brace
  }
  state_.session.clear();
  state_.usage.reset();
  // Clearing in place is what makes this work for whichever backend is live:
  // it reads its memory through the pointer the dialog still holds. Only the
  // ACTIVE key's memory is touched — the other backend's last conversation is
  // none of "New chat"'s business.
  *memoryFor(active_backend_key_) = HarnessMemory{};
  state_.active_conversation_id.clear();
  // The persisted pointer goes too: New chat is the one gesture that frees
  // the user from the past, and a reopen must not resume it. Nothing is
  // deleted on disk — the old conversation stays in the harness's store and
  // the drawer, just no longer active.
  SettingsStore store(settings_);
  clearActiveSessionId(store, active_backend_key_);
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

  const std::shared_ptr<HarnessMemory> memory = memoryFor(active_backend_key_);
  *memory = HarnessMemory{};
  memory->session_id = id;
  // Forces composePayload to re-send the catalog with the resume note on the
  // next turn: this process's ephemeral state (tabs the model composed, etc.)
  // died with whatever process wrote this transcript, and --resume would
  // otherwise replay history as if it hadn't.
  memory->resumed_pending = true;

  state_.active_conversation_id = id;
  SettingsStore store(settings_);
  saveActiveSessionId(store, id, active_backend_key_);

  // The drawer stays open: it is a sidebar the user browses, not a menu that
  // dismisses itself once picked from.
  state_.drawer_dirty = true;
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
  return true;
}

void AssistantDialog::loadPersistedConversation() {
  // rebuildBackend() must already have run against these same settings — a
  // persisted Codex id needs to be loaded THROUGH a CodexBackend, not the
  // ctor's default ClaudeBackend (backend_ built against an unbound store).
  activateBackendConversation();
}

void AssistantDialog::activateBackendConversation() {
  SettingsStore store(settings_);
  const std::string id = loadActiveSessionId(store, active_backend_key_);
  if (id.empty() || !switchToConversation(id)) {
    // Nothing was persisted for this backend, or it was purged since the last
    // time it was active either way, there is no --resume target to dangle.
    clearActiveSessionId(store, active_backend_key_);
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.session.clear();
    state_.usage.reset();
    state_.active_conversation_id.clear();
  }
  // switchToConversation() on success already sets transcript/controls dirty,
  // but setting them again here is free, and it is what covers the
  // blank-reset branch above too -- every path through this function leaves
  // the panel showing exactly what is now active. The drawer's listing is
  // always visible now, so it is always refreshed here too.
  std::lock_guard<std::mutex> lock(state_.mu);
  refreshConversationsLocked();
  state_.drawer_dirty = true;
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
}

void AssistantDialog::refreshConversationsLocked() {
  state_.conversations = backend_ ? backend_->listConversations() : std::vector<ConversationSummary>{};
  std::vector<std::string> ids;
  ids.reserve(state_.conversations.size());
  for (const ConversationSummary& c : state_.conversations) {
    ids.push_back(c.id);
  }
  // Prune first, then reload: a rename whose conversation just fell out of
  // this same listing must not still show up in conversation_titles below.
  SettingsStore store(settings_);
  pruneConversationTitles(store, active_backend_key_, ids);
  state_.conversation_titles = loadConversationTitles(store, active_backend_key_);
}

std::string AssistantDialog::idForListTextLocked(const std::string& text) const {
  for (const ConversationSummary& c : state_.conversations) {
    if (listText(c, state_.conversation_titles) == text) {
      return c.id;
    }
  }
  return {};
}

std::string AssistantDialog::listTextForIdLocked(const std::string& id) const {
  for (const ConversationSummary& c : state_.conversations) {
    if (c.id == id) {
      return listText(c, state_.conversation_titles);
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
    // rebuildBackend() FIRST: the ctor already built a default ClaudeBackend
    // against an unbound settings view, but a persisted choice of "codex"
    // needs its conversation loaded THROUGH a CodexBackend (loadTranscript
    // reads a different store) — loadPersistedConversation() below relies on
    // active_backend_key_ already reflecting the real, bound settings.
    rebuildBackend();
    loadPersistedConversation();
    return;
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
      !state_.open_settings_pending && !state_.open_rename_pending && !state_.drawer_dirty &&
      !state_.header_icons_pending) {
    return {};
  }
  // SubDialogKind's backstop: a Cancel on either sub-dialog never fires
  // subDialogAccepted, so nothing else clears open_sub_dialog. The moment
  // THIS render has no fresh request of its own, whatever it was pointing at
  // is stale.
  if (!state_.open_settings_pending && !state_.open_rename_pending) {
    state_.open_sub_dialog = SubDialogKind::None;
  }
  PJ::WidgetData wd;

  // Every widget-data object that names "conversationList" must also carry
  // list_deletable, or the host turns the trash/elision delegate off for that
  // payload (widget_data.hpp: "re-send on every build") -- both blocks below
  // that touch this widget route through here instead of repeating the flag.
  const auto nameConversationList = [&wd](bool busy) {
    wd.setEnabled("conversationList", !busy);
    wd.setListItemsDeletable("conversationList", true);
  };

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
    nameConversationList(busy);
    state_.controls_dirty = false;
  }

  if (state_.header_icons_pending) {
    // A static per-panel glyph; no reason to resend on every build like the
    // per-turn state above.
    wd.setButtonIconNamed("newChatButton", "add");
    state_.header_icons_pending = false;
  }

  if (state_.drawer_dirty) {
    nameConversationList(state_.session.busy());
    std::vector<std::string> rows;
    rows.reserve(state_.conversations.size());
    for (const ConversationSummary& c : state_.conversations) {
      rows.push_back(listText(c, state_.conversation_titles));
    }
    wd.setListItems("conversationList", rows);
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
    state_.open_sub_dialog = SubDialogKind::Settings;
    // Pre-fill the modal from persisted settings before showing it.
    SettingsStore store(settings_);
    const BackendSpec* stored_spec = backendByKey(store.getString(kKeyBackend, "claude"));
    wd.setCurrentIndex("backendCombo", stored_spec ? stored_spec->combo_index : 0);
    for (const BackendSpec& spec : kBackends) {
      wd.setText(widgetName(spec, "CliPathEdit"), store.getString(spec.cli_key, spec.default_cli));

      // Model combo: "CLI default", "Custom...", then this backend's own
      // models() -- ModelPicker owns the index<->value mapping both ways.
      const ModelPicker picker(spec.models());
      const std::string model_combo = widgetName(spec, "ModelCombo");
      wd.setItems(model_combo, picker.items());

      const std::string value = store.getString(spec.model_key, spec.default_model);
      const int index = picker.indexForValue(value);
      wd.setCurrentIndex(model_combo, index);
      wd.setText(widgetName(spec, "ModelEdit"), index == ModelPicker::kIndexCustom ? value : "");
    }
    wd.requestSubDialog(kAssistantSettingsUi);
  }

  if (state_.open_rename_pending) {
    state_.open_rename_pending = false;
    state_.open_sub_dialog = SubDialogKind::Rename;
    wd.setText("renameEdit", state_.rename_prefill);
    wd.requestSubDialog(kRenameConversationUi);
  }

  return wd.toJson();
}

bool AssistantDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "inputEdit") {
    state_.input_text = std::string(text);
    return false;  // no re-render; the widget already shows the text
  }
  if (widget_name == "renameEdit") {
    // Staged exactly like a settings field: the host harvests this box's
    // current text on accept regardless of whether the user typed anything,
    // so this always lands before subDialogAccepted fires.
    state_.pending_rename_text = std::string(text);
    return false;
  }
  // Settings sub-dialog inputs: stage the value under its settings key; commit
  // on subDialogAccepted. Widget names derived from each BackendSpec's key
  // (widgetName()) instead of a hand-written table.
  for (const BackendSpec& spec : kBackends) {
    if (widget_name == widgetName(spec, "ModelEdit")) {
      state_.pending_text[spec.model_key] = std::string(text);
      break;
    }
    if (widget_name == widgetName(spec, "CliPathEdit")) {
      state_.pending_text[spec.cli_key] = std::string(text);
      break;
    }
  }
  return false;
}

bool AssistantDialog::onIndexChanged(std::string_view widget_name, int index) {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "backendCombo") {
    // Stage only; commitSettings() persists it under kKeyBackend. "Claude Code
    // (subscription)" is combo index 0, "Codex (ChatGPT subscription)" is 1
    // (ui/assistant_settings.ui) — an index this build has never heard of
    // falls back to "claude" rather than staging a value nothing downstream
    // understands.
    const BackendSpec* spec = backendByIndex(index);
    state_.pending_backend = spec ? spec->key : "claude";
    return false;
  }
  // A model combo. The host harvests every QLineEdit before any QComboBox on
  // accept, so onTextChanged(model_edit, ...) has already staged whatever
  // that box currently shows into pending_text[model_key] by the time this
  // runs — which is exactly what "Custom..." (ModelPicker::valueForIndex
  // returning nullopt) wants persisted, so that case below leaves it alone.
  for (const BackendSpec& spec : kBackends) {
    if (widget_name != widgetName(spec, "ModelCombo")) {
      continue;
    }
    const ModelPicker picker(spec.models());
    if (const std::optional<std::string> value = picker.valueForIndex(index); value.has_value()) {
      state_.pending_text[spec.model_key] = *value;
    }
    break;
  }
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
  if (widget_name == "subDialogAccepted") {
    // Exactly one sub-dialog can be open at a time, and PanelEngine fires this
    // same synthetic click for whichever one the user just accepted (host
    // side: panel_engine.cpp) -- route by what widget_data() last requested,
    // not by guessing from which fields happen to be staged.
    SubDialogKind kind;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      kind = state_.open_sub_dialog;
      state_.open_sub_dialog = SubDialogKind::None;
    }
    if (kind == SubDialogKind::Rename) {
      commitRename();
    } else {
      // Settings, or (should never happen) None -- the safe default is the
      // one every build before this one always did.
      commitSettings();
    }
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

bool AssistantDialog::onItemContextAction(std::string_view widget_name, int index, std::string_view action_id) {
  if (widget_name != "conversationList" || action_id != "rename") {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  if (state_.session.busy()) {
    return false;  // the list is disabled while busy; this is the belt to that brace
  }
  if (index < 0 || static_cast<std::size_t>(index) >= state_.conversations.size()) {
    return false;  // a stale row index racing a delete
  }
  const ConversationSummary& c = state_.conversations[static_cast<std::size_t>(index)];
  state_.rename_conversation_id = c.id;
  state_.rename_prefill = displayTitle(c, state_.conversation_titles);
  state_.pending_rename_text.reset();
  state_.open_rename_pending = true;
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
    // This is the other half of commitSettings()'s own call: a Settings
    // commit that landed mid-turn defers rebuildBackend() (see its doc
    // comment), so the backend switch — and the conversation switch that
    // goes with it — only actually happens here, once the turn is over.
    if (rebuildBackend()) {
      activateBackendConversation();
    }
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
    case BackendEvent::Kind::TurnComplete: {
      state_.session.setState(TurnState::Idle);
      state_.controls_dirty = true;
      // Persist the active session id once it actually changes — a brand-new
      // conversation gets its id here for the first time; a resumed one
      // already has it saved, so this is then a no-op read. Always the
      // memory for the backend that actually ran this turn (active_backend_key_
      // cannot have changed mid-turn: rebuildBackend() defers itself while busy).
      const std::shared_ptr<HarnessMemory> memory = memoryFor(active_backend_key_);
      if (!memory->session_id.empty() && memory->session_id != state_.active_conversation_id) {
        state_.active_conversation_id = memory->session_id;
        SettingsStore store(settings_);
        saveActiveSessionId(store, state_.active_conversation_id, active_backend_key_);
      }
      break;
    }
  }
}

void AssistantDialog::commitSettings() {
  SettingsStore store(settings_);
  std::string backend;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    // Persist each staged text edit and clear the map for the next time the
    // modal opens.
    for (const auto& [key, value] : state_.pending_text) {
      store.setString(key, value);
    }
    state_.pending_text.clear();
    if (state_.pending_backend.has_value()) {
      store.setString(kKeyBackend, *state_.pending_backend);
      state_.pending_backend.reset();
    }
    backend = store.getString(kKeyBackend, "claude");
  }

  // Re-derive the header label + active backend from the freshly persisted
  // choice. If a turn is in flight, rebuildBackend() defers itself and
  // returns false (see its doc comment) — onTick applies the deferred
  // rebuild and this same activation once the turn is over. Before the
  // "Settings saved" note, so that note lands in the transcript it is about
  // (the backend just switched TO), not the one just left.
  if (rebuildBackend()) {
    activateBackendConversation();
  }

  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.session.addSystem("Settings saved (backend: " + backend + ").");
    state_.transcript_dirty = true;
  }

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

void AssistantDialog::commitRename() {
  std::string id;
  std::string text;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    id = state_.rename_conversation_id;
    text = state_.pending_rename_text.value_or(std::string());
    state_.rename_conversation_id.clear();
    state_.rename_prefill.clear();
    state_.pending_rename_text.reset();
  }
  if (id.empty()) {
    return;  // defensive: nothing staged to commit (should not happen, see onItemContextAction)
  }
  SettingsStore store(settings_);
  if (isBlank(text)) {
    removeConversationTitle(store, active_backend_key_, id);
  } else {
    setConversationTitle(store, active_backend_key_, id, text);
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  // Only the affected id needs updating, but reloading the whole map is one
  // read and keeps this in lockstep with refreshConversationsLocked() instead
  // of hand-duplicating its logic here.
  state_.conversation_titles = loadConversationTitles(store, active_backend_key_);
  state_.drawer_dirty = true;  // the renamed row's text just changed
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
