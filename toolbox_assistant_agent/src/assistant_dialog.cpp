// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "assistant_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "assistant_panel_manifest.hpp"
#include "assistant_panel_ui.hpp"
#include "assistant_settings_ui.hpp"
#include "catalog_digest.hpp"
#include "claude_backend.hpp"
#include "codex_backend.hpp"
#include "conversation_state.hpp"
#include "features_digest.hpp"
#include "model_picker.hpp"
#include "rename_conversation_ui.hpp"
#include "settings_store.hpp"

namespace assistant_agent {

namespace {
// Persisted settings keys (pj.settings.v1), namespaced so they never collide with another
// toolbox's keys in the shared store.
constexpr const char* kKeyBackend = "assistant.backend";  // "claude" or "codex"
// Catalog digest budget (catalogDigest); the default equals catalogDigest's own, and
// resolveCatalogBudgetChars clamps so a stray stored value cannot blow it up or shrink it to nothing.
constexpr const char* kKeyCatalogBudgetChars = "assistant.catalog_budget_chars";
constexpr int kDefaultCatalogBudgetChars = 10000;
constexpr int kMinCatalogBudgetChars = 1000;
constexpr int kMaxCatalogBudgetChars = 200000;
// Same for the "PlotJuggler features" block, whose default is featuresDigest's own.
constexpr const char* kKeyFeaturesBudgetChars = "assistant.features_budget_chars";
constexpr int kMaxFeaturesBudgetChars = 200000;

// One row per backend: its settings key, the backendCombo index it fills, the settings
// keys/defaults of its model/cli-path fields, and two function pointers: `make` builds the
// backend (rebuildBackend()) and `models` lists its combo entries, so neither needs a switch on
// `key` or a throwaway instance. Widget names are derived by widgetName().
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
    // A blank default would pass no --model and get the CLI's own default, the slowest tier for no
    // measurable gain: `sonnet` matches the fastest tier on turn time and was the only tier with
    // no miss in the 240-cell study. A user who clears the field still gets the CLI default, since
    // an empty stored value is returned as-is.
    {"claude", 0, "assistant.claude.model", "assistant.claude.cli_path", "sonnet", "claude", &makeClaudeBackend,
     &ClaudeBackend::listModels},
    // No benchmark pick for Codex: empty means whatever `codex exec` chooses.
    {"codex", 1, "assistant.codex.model", "assistant.codex.cli_path", "", "codex", &makeCodexBackend,
     &CodexBackend::listModels},
};

// A BackendSpec field's widget name, e.g. widgetName(kBackends[0], "ModelCombo") ==
// "claudeModelCombo" (ui/assistant_settings.ui).
std::string widgetName(const BackendSpec& spec, const char* suffix) {
  return std::string(spec.key) + suffix;
}

// nullptr for a choice this build does not know.
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

// The user's custom name (conversation_state.hpp) if set, else the harness's title.
std::string displayTitle(const ConversationSummary& c, const ConversationTitles& titles) {
  const auto it = titles.find(c.id);
  return it != titles.end() ? it->second : c.title;
}

// A drawer row's text. The date suffix keeps two same-titled conversations apart under the
// host's text-keyed list protocol (setSelectedItems / onSelectionChanged match by TEXT).
std::string listText(const ConversationSummary& c, const ConversationTitles& titles) {
  return displayTitle(c, titles) + " · " + formatShortDate(c.last_ts);
}

// An empty or whitespace-only rename resets the name rather than storing "".
bool isBlank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}

// The backend key a persisted `assistant.backend` value resolves to; "echo" for an unknown
// choice (no store of its own, so its memory is never resumed into).
std::string resolveBackendKey(const SettingsStore& store) {
  const std::string choice = store.getString(kKeyBackend, "claude");
  return backendByKey(choice) ? choice : "echo";
}

int clampedSetting(const SettingsStore& store, const char* key, int def, int lo, int hi) {
  return std::clamp(store.getInt(key, def), lo, hi);
}
}  // namespace

int resolveCatalogBudgetChars(const SettingsStore& store) {
  return clampedSetting(
      store, kKeyCatalogBudgetChars, kDefaultCatalogBudgetChars, kMinCatalogBudgetChars, kMaxCatalogBudgetChars);
}

int resolveFeaturesBudgetChars(const SettingsStore& store) {
  return clampedSetting(
      store, kKeyFeaturesBudgetChars, static_cast<int>(kDefaultFeaturesBudgetChars),
      static_cast<int>(kMinFeaturesBudgetChars), kMaxFeaturesBudgetChars);
}

AssistantDialog::AssistantDialog() {
  rebuildBackend();
  worker_thread_ = std::thread([this]() { workerLoop(); });
}

AssistantDialog::~AssistantDialog() {
  // Abort an in-flight turn first: the worker may be blocked in a subprocess wait, and join()
  // would stall teardown until it finished.
  if (backend_) {
    backend_->cancel();
  }
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
  // Never mid-turn: the incoming and outgoing backends share the conversation memory, which the
  // worker may be writing a session id into. The Settings button is disabled while busy, so this
  // only catches a host-driven setSettings() during a turn. Deferred, it returns false; onTick
  // applies the rebuild after the turn and acts on that call's return value.
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.session.busy()) {
      state_.rebuild_pending = true;
      return false;
    }
    state_.rebuild_pending = false;
  }

  const std::string previous_key = active_backend_key_;

  // "claude" and "codex" each get their own conversation memory (memoryFor); an unknown choice
  // falls back to the visibly labeled echo backend.
  SettingsStore store(settings_);
  active_backend_key_ = resolveBackendKey(store);
  if (const BackendSpec* spec = backendByKey(active_backend_key_)) {
    backend_ = spec->make(
        store.getString(spec->cli_key, spec->default_cli), store.getString(spec->model_key, spec->default_model),
        memoryFor(spec->key));
  } else {
    backend_ = std::make_shared<EchoBackend>();
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.backend_name = backend_->name();
  state_.controls_dirty = true;  // the status line names the backend
  return active_backend_key_ != previous_key;
}

void AssistantDialog::startNewConversation() {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (state_.session.busy()) {
    return;  // the button is disabled while busy
  }
  state_.session.clear();
  state_.usage.reset();
  // Cleared in place: the live backend reads its memory through the pointer the dialog holds.
  // Only the ACTIVE key's memory is touched.
  *memoryFor(active_backend_key_) = HarnessMemory{};
  state_.active_conversation_id.clear();
  // The persisted id goes too, so a reopen does not resume it. Nothing is deleted on disk.
  SettingsStore store(settings_);
  clearActiveSessionId(store, active_backend_key_);
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
  state_.drawer_dirty = true;
}

bool AssistantDialog::switchToConversation(const std::string& id) {
  const std::vector<ChatMessage> transcript = backend_ ? backend_->loadTranscript(id) : std::vector<ChatMessage>{};
  if (transcript.empty()) {
    return false;  // purged, or a stale drawer row
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
        // appendAssistant merges consecutive blocks into one row, like a live streamed reply.
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
  // The model's memory returns through --resume regardless; this lets the user SEE the seam.
  state_.session.addSystem("resumed previous conversation");

  const std::shared_ptr<HarnessMemory> memory = memoryFor(active_backend_key_);
  *memory = HarnessMemory{};
  memory->session_id = id;
  // Makes composePayload re-send the catalog with the resume note: the loaded layout and owned
  // tabs may differ from this transcript, while --resume replays the earlier state.
  memory->resumed_pending = true;

  state_.active_conversation_id = id;
  SettingsStore store(settings_);
  saveActiveSessionId(store, id, active_backend_key_);

  state_.drawer_dirty = true;
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
  return true;
}

void AssistantDialog::loadPersistedConversation() {
  // rebuildBackend() must already have run against these settings: a persisted Codex id has to
  // load THROUGH a CodexBackend, not the ctor's default ClaudeBackend.
  activateBackendConversation();
}

void AssistantDialog::activateBackendConversation() {
  SettingsStore store(settings_);
  const std::string id = loadActiveSessionId(store, active_backend_key_);
  if (id.empty() || !switchToConversation(id)) {
    // Nothing persisted, or purged since: leave no dangling --resume target.
    clearActiveSessionId(store, active_backend_key_);
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.session.clear();
    state_.usage.reset();
    state_.active_conversation_id.clear();
  }
  // Also covers the blank-reset branch: every path leaves the panel showing what is now active.
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
  // Prune before reloading, so a rename whose conversation just fell out of this listing is gone.
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

void AssistantDialog::setCatalogHosts(PJ::sdk::ToolboxHostView host, PJ::sdk::PlaybackHostView playback) {
  toolbox_host_ = host;
  playback_host_ = playback;
}

void AssistantDialog::setMcpEndpointHost(PJ::sdk::McpEndpointHostView host) {
  mcp_host_ = host;
}

void AssistantDialog::setFeatureCatalogProvider(std::function<PJ::sdk::FeatureCatalogView()> provider) {
  feature_catalog_provider_ = std::move(provider);
}

std::string AssistantDialog::featuresForTurn() {
  if (!feature_catalog_provider_) {
    return {};
  }
  const SettingsStore store(settings_);
  return featuresBlock(feature_catalog_provider_(), static_cast<std::size_t>(resolveFeaturesBudgetChars(store)));
}

void AssistantDialog::setSettings(PJ::sdk::SettingsView settings) {
  settings_ = settings;
  if (!conversation_loaded_) {
    conversation_loaded_ = true;
    // First bind: the store is readable, so retire Ollama-era keys before anything reads the
    // backend choice.
    SettingsStore store(settings_);
    if (store.getString(kKeyBackend, "") == "ollama") {
      store.setString(kKeyBackend, "claude");
    }
    scrubRetiredOllamaKeys(store);
    scrubLegacyConversationKeys(store);
    // rebuildBackend() FIRST: the ctor built a default ClaudeBackend against an unbound view, but
    // a persisted "codex" conversation must load THROUGH a CodexBackend (a different store);
    // loadPersistedConversation() relies on active_backend_key_ reflecting the bound settings.
    rebuildBackend();
    loadPersistedConversation();
    return;
  }
  rebuildBackend();
}

std::string AssistantDialog::widget_data() {
  std::lock_guard<std::mutex> lock(state_.mu);
  // Steady state: skip building a WidgetData 20x/sec; the host treats an empty return as "no update".
  if (!state_.transcript_dirty && !state_.controls_dirty && !state_.clear_input_pending &&
      !state_.open_settings_pending && !state_.open_rename_pending && !state_.drawer_dirty &&
      !state_.header_icons_pending) {
    return {};
  }
  // SubDialogKind's backstop: a Cancel never fires subDialogAccepted, so a render with no fresh
  // request means open_sub_dialog is stale.
  if (!state_.open_settings_pending && !state_.open_rename_pending) {
    state_.open_sub_dialog = SubDialogKind::None;
  }
  PJ::WidgetData wd;

  // Every payload naming "conversationList" must also carry list_deletable, or the host turns the
  // trash/elision delegate off for that payload (widget_data.hpp: "re-send on every build").
  const auto nameConversationList = [&wd](bool busy) {
    wd.setEnabled("conversationList", !busy);
    wd.setListItemsDeletable("conversationList", true);
  };

  if (state_.transcript_dirty) {
    wd.setPlainText("transcriptText", state_.session.renderMarkdown());
    state_.transcript_dirty = false;
  }

  if (state_.controls_dirty) {
    const bool busy = state_.session.busy();
    // Turn state, answering backend, then the last turn's usage (summary() may be "").
    wd.setLabel(
        "statusLabel", state_.session.statusText() + "  \xc2\xb7  " + state_.backend_name + state_.usage.summary());
    wd.setEnabled("inputEdit", !busy);
    wd.setEnabled("sendButton", !busy);
    wd.setEnabled("cancelButton", busy);
    // A mid-turn backend rebuild would strand the turn on the old backend.
    wd.setEnabled("settingsButton", !busy);
    // Also: wiping the memory under a running turn would resume a session it just forgot.
    wd.setEnabled("newChatButton", !busy);
    nameConversationList(busy);
    state_.controls_dirty = false;
  }

  if (state_.header_icons_pending) {
    // Static glyph; not resent on every build.
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
    // Pre-fill the modal from persisted settings.
    SettingsStore store(settings_);
    const BackendSpec* stored_spec = backendByKey(store.getString(kKeyBackend, "claude"));
    wd.setCurrentIndex("backendCombo", stored_spec ? stored_spec->combo_index : 0);
    for (const BackendSpec& spec : kBackends) {
      wd.setText(widgetName(spec, "CliPathEdit"), store.getString(spec.cli_key, spec.default_cli));

      // ModelPicker owns the combo's index<->value mapping both ways.
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
    return false;  // the widget already shows the text
  }
  if (widget_name == "renameEdit") {
    // Staged like a settings field: the host harvests this box's text on accept even if untouched,
    // so it always lands before subDialogAccepted.
    state_.pending_rename_text = std::string(text);
    return false;
  }
  // Settings sub-dialog inputs: staged under their settings key, committed on subDialogAccepted.
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
    // Staged only; commitSettings() persists it. Combo index 0 is Claude Code, 1 is Codex
    // (ui/assistant_settings.ui); an unknown index falls back to "claude".
    const BackendSpec* spec = backendByIndex(index);
    state_.pending_backend = spec ? spec->key : "claude";
    return false;
  }
  // A model combo. The host harvests every QLineEdit before any QComboBox on accept, so
  // onTextChanged(model_edit, ...) has already staged the box's text into pending_text[model_key];
  // "Custom..." (valueForIndex returning nullopt) wants exactly that, so it is left alone.
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
    // Stops the CLI, then releases the endpoint: PlotJuggler revokes its token, which stops
    // anything the CLI left running there (a backgrounded import or evaluation). The next turn
    // acquires a new endpoint; the turn holds its own copy, so the worker shares nothing with this.
    if (backend_) {
      backend_->cancel();
    }
    if (mcp_host_.valid()) {
      mcp_host_.release();
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
    // One sub-dialog is open at a time and PanelEngine fires this same synthetic click for either;
    // route by what widget_data() last requested, not by which fields are staged.
    SubDialogKind kind;
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      kind = state_.open_sub_dialog;
      state_.open_sub_dialog = SubDialogKind::None;
    }
    if (kind == SubDialogKind::Rename) {
      commitRename();
    } else {
      // Settings, or None (should not happen).
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
      return false;  // the active row again, or a row that raced with a delete
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
      return false;  // the list is disabled while busy
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
    // Not a blank dead end: also clears the active id and the in-memory session/memory.
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
    return false;  // the list is disabled while busy
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
  // Worker-posted closures mutate state_ and set dirty flags, so they run here on the GUI thread.
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(evt_mu_);
    batch.swap(evt_queue_);
  }
  for (auto& fn : batch) {
    fn();
  }

  // A rebuild deferred until the turn ended. Read under the lock, but rebuildBackend() runs
  // outside it: it takes the same non-recursive mutex and re-checks busy() itself.
  bool rebuild = false;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    rebuild = state_.rebuild_pending && !state_.session.busy();
  }
  if (rebuild) {
    // The other half of commitSettings()'s call: a commit mid-turn defers the backend switch (and
    // its conversation switch) to here.
    if (rebuildBackend()) {
      activateBackendConversation();
    }
  }

  return !batch.empty() || rebuild;
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

  // The worker cannot call host services, so everything host-side is snapshotted here on the GUI
  // thread. The catalog is rebuilt every turn (cheap host metadata) so a newly loaded file or
  // stream shows up; the backend decides whether this turn carries it. The budget is also read
  // fresh each turn.
  TurnTools tools;
  if (toolbox_host_.valid()) {
    const SettingsStore store(settings_);
    tools.catalog =
        catalogDigest(toolbox_host_, static_cast<std::size_t>(resolveCatalogBudgetChars(store)), playback_host_);
  }
  // Rebuilt every turn too, so an enabled or installed plugin shows up (its own hash decides
  // whether it is re-sent).
  tools.features = featuresForTurn();
  // Acquired here because the host service is main-thread only. A failure is passed on: the
  // backend fails the turn with it, in the chat.
  tools.mcp = PJ::unexpected("it offers no MCP endpoint to plugins.");
  if (mcp_host_.valid()) {
    tools.mcp = mcp_host_.acquire();
  }
  if (!tools.mcp) {
    tools.mcp = PJ::unexpected("This PlotJuggler cannot serve the Assistant's tools: " + tools.mcp.error());
  }

  postCommand([this, text, tools = std::move(tools), backend = backend_]() {
    LlmBackend::EventSink sink = [this](BackendEvent ev) { postEvent([this, ev]() { applyBackendEvent(ev); }); };
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
      // Append, not add: a streaming backend sends one reply as many chunks.
      state_.session.appendAssistant(ev.text);
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::ToolActivity:
      state_.session.addTool(ev.text);
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::ToolResult:
      break;  // not rendered: the call already has its ToolActivity row
    case BackendEvent::Kind::Error:
      state_.session.addSystem("Error: " + ev.text);
      if (ev.resume_failed) {
        // --resume itself failed (session purged between listing and this turn): say what the error
        // text does not, that retrying will not help.
        state_.session.addSystem("This conversation can no longer be continued; start a new one.");
      }
      state_.transcript_dirty = true;
      break;
    case BackendEvent::Kind::Metrics:
      // No repaint of its own: TurnComplete follows and re-renders the status label.
      state_.usage.record(ev.metrics);
      break;
    case BackendEvent::Kind::TurnComplete: {
      state_.session.setState(TurnState::Idle);
      state_.controls_dirty = true;
      // Persist the session id once it changes (a new conversation gets its id here). Always the
      // memory of the backend that ran this turn: active_backend_key_ cannot change mid-turn.
      const std::shared_ptr<HarnessMemory> memory = memoryFor(active_backend_key_);
      if (!memory->session_id.empty() && memory->session_id != state_.active_conversation_id) {
        state_.active_conversation_id = memory->session_id;
        SettingsStore store(settings_);
        saveActiveSessionId(store, state_.active_conversation_id, active_backend_key_);
      }
      refreshConversationsLocked();
      state_.drawer_dirty = true;
      break;
    }
  }
}

void AssistantDialog::commitSettings() {
  SettingsStore store(settings_);
  std::string backend;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
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

  // Before the "Settings saved" note, so it lands in the transcript of the backend just switched
  // TO. If a turn is in flight rebuildBackend() defers and returns false; onTick then applies the
  // rebuild and this activation.
  if (rebuildBackend()) {
    activateBackendConversation();
  }

  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.session.addSystem("Settings saved (backend: " + backend + ").");
    state_.transcript_dirty = true;
  }

  // Probe off the GUI thread; the captured reference keeps a later rebuild from swapping the
  // object out from under the probe.
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
    return;  // nothing staged (onItemContextAction always stages one)
  }
  SettingsStore store(settings_);
  if (isBlank(text)) {
    removeConversationTitle(store, active_backend_key_, id);
  } else {
    setConversationTitle(store, active_backend_key_, id, text);
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.conversation_titles = loadConversationTitles(store, active_backend_key_);
  state_.drawer_dirty = true;
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
