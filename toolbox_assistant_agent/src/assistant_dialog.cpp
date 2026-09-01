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
#include "ollama_backend.hpp"
#include "settings_store.hpp"

namespace assistant_agent {

namespace {
// Persisted settings keys (pj.settings.v1). Namespaced so they never collide
// with another toolbox's keys in the shared store.
constexpr const char* kKeyBackend = "assistant.backend";  // "ollama" | "claude"
constexpr const char* kKeyOllamaUrl = "assistant.ollama.url";
constexpr const char* kKeyOllamaModel = "assistant.ollama.model";
constexpr const char* kKeyClaudeModel = "assistant.claude.model";
constexpr const char* kKeyClaudeCli = "assistant.claude.cli_path";

constexpr const char* kDefaultOllamaUrl = "http://localhost:11434";

// Leaving this blank would pass no --model and get the CLI's own default, which
// is the slowest tier for no measurable gain: `sonnet` matches the fastest tier
// on turn time (10.1 s vs 10.4 s median) and was the only tier with no miss in
// the 240-cell study (docs/BENCHMARKS.md). A user who deliberately clears the
// field still gets the CLI default, because an empty stored value is returned
// as-is rather than falling back to this.
constexpr const char* kDefaultClaudeModel = "sonnet";
}  // namespace

AssistantDialog::AssistantDialog()
    : claude_memory_(std::make_shared<ClaudeMemory>()), ollama_memory_(std::make_shared<OllamaMemory>()) {
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
  // 'assistant.backend' choice selects the real backend; an unset/unknown choice
  // falls back to the harmless echo backend.
  const char* fake = std::getenv("ASSISTANT_FAKE_BACKEND");
  if (fake != nullptr && std::string(fake) == "1") {
    backend_ = std::make_shared<FakeBackend>();
  } else {
    SettingsStore store(settings_);
    const std::string choice = store.getString(kKeyBackend, "");
    if (choice == "ollama") {
      backend_ = std::make_shared<OllamaBackend>(
          store.getString(kKeyOllamaUrl, kDefaultOllamaUrl), store.getString(kKeyOllamaModel, ""), ollama_memory_);
    } else if (choice == "claude") {
      backend_ = std::make_shared<ClaudeBackend>(
          store.getString(kKeyClaudeCli, "claude"), store.getString(kKeyClaudeModel, kDefaultClaudeModel),
          claude_memory_);
    } else {
      // Unset choice -> the harmless echo backend until the user picks one.
      backend_ = std::make_shared<EchoBackend>();
    }
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.backend_name = backend_->name();
  state_.header_dirty = true;
}

void AssistantDialog::startNewConversation() {
  std::lock_guard<std::mutex> lock(state_.mu);
  if (state_.session.busy()) {
    return;  // the button is disabled while busy; this is the belt to that brace
  }
  state_.session.clear();
  state_.usage.reset();
  // Clearing in place is what makes this work for whichever backend is live:
  // both read their memory through the pointer the dialog still holds.
  *claude_memory_ = ClaudeMemory{};
  *ollama_memory_ = OllamaMemory{};
  // The persisted copy goes too: New chat is the one gesture that frees the
  // user from the past, and a reopen must not resurrect it.
  SettingsStore store(settings_);
  eraseConversation(store);
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
}

void AssistantDialog::loadPersistedConversation() {
  SettingsStore store(settings_);
  const ConversationState conv = loadConversation(store);
  if (conv.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  for (const ChatMessage& m : conv.messages) {
    switch (m.role) {
      case ChatMessage::Role::User:
        state_.session.addUser(m.text);
        break;
      case ChatMessage::Role::Assistant:
        // addAssistant, not appendAssistant: rows were persisted as final
        // messages and must come back as the same rows, not merged.
        state_.session.addAssistant(m.text);
        break;
      case ChatMessage::Role::System:
        state_.session.addSystem(m.text);
        break;
      case ChatMessage::Role::Tool:
        state_.session.addTool(m.text);
        break;
    }
  }
  if (!conv.messages.empty()) {
    // Visible seam between then and now — the model's memory comes back through
    // --resume regardless; this keeps the user able to SEE that it did.
    state_.session.addSystem("resumed previous conversation");
  }
  claude_memory_->session_id = conv.claude_session_id;
  claude_memory_->sent_catalog_hash = conv.claude_catalog_hash;
  if (!conv.ollama_history_json.empty()) {
    nlohmann::json parsed = nlohmann::json::parse(conv.ollama_history_json, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_array()) {
      ollama_memory_->history = std::move(parsed);
    }
  }
  state_.transcript_dirty = true;
  state_.controls_dirty = true;
}

void AssistantDialog::saveConversationLocked() {
  ConversationState conv;
  conv.messages = state_.session.messages();
  conv.claude_session_id = claude_memory_->session_id;
  conv.claude_catalog_hash = claude_memory_->sent_catalog_hash;
  if (!ollama_memory_->history.empty()) {
    conv.ollama_history_json = ollama_memory_->history.dump();
  }
  SettingsStore store(settings_);
  saveConversation(store, conv);
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

void AssistantDialog::setSettings(PJ::sdk::SettingsView settings) {
  settings_ = settings;
  if (!conversation_loaded_) {
    conversation_loaded_ = true;
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
  if (!state_.header_dirty && !state_.transcript_dirty && !state_.controls_dirty && !state_.clear_input_pending &&
      !state_.open_settings_pending) {
    return {};
  }
  PJ::WidgetData wd;

  if (state_.header_dirty) {
    wd.setLabel("backendLabel", "Backend: " + state_.backend_name);
    state_.header_dirty = false;
  }

  if (state_.transcript_dirty) {
    wd.setPlainText("transcriptText", state_.session.render());
    state_.transcript_dirty = false;
  }

  if (state_.controls_dirty) {
    const bool busy = state_.session.busy();
    // The turn state, then what the last turn moved. summary() returns "" when
    // there is nothing to report, which is the whole story for a local backend
    // — no branch on which backend is live.
    wd.setLabel("statusLabel", state_.session.statusText() + state_.usage.summary());
    wd.setEnabled("inputEdit", !busy);
    wd.setEnabled("sendButton", !busy);
    wd.setEnabled("cancelButton", busy);
    // A mid-turn backend rebuild would strand the in-flight turn on the old
    // backend (kept alive by the worker's reference) — just don't offer it.
    wd.setEnabled("settingsButton", !busy);
    // Same reason, plus: wiping the conversation memory under a running turn
    // would have the backend resume a session it just forgot.
    wd.setEnabled("newChatButton", !busy);
    state_.controls_dirty = false;
  }

  if (state_.clear_input_pending) {
    wd.setText("inputEdit", "");
    state_.clear_input_pending = false;
  }

  if (state_.open_settings_pending) {
    state_.open_settings_pending = false;
    // Pre-fill the modal from persisted settings before showing it.
    SettingsStore store(settings_);
    const std::string backend = store.getString(kKeyBackend, "ollama");
    wd.setCurrentIndex("backendCombo", backend == "claude" ? 1 : 0);
    wd.setText("ollamaUrlEdit", store.getString(kKeyOllamaUrl, kDefaultOllamaUrl));
    wd.setText("ollamaModelEdit", store.getString(kKeyOllamaModel, ""));
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
  if (widget_name == "ollamaUrlEdit") {
    state_.pending_ollama_url = std::string(text);
    return false;
  }
  if (widget_name == "ollamaModelEdit") {
    state_.pending_ollama_model = std::string(text);
    return false;
  }
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
  std::lock_guard<std::mutex> lock(state_.mu);
  if (widget_name == "backendCombo") {
    state_.pending_backend = (index == 1) ? "claude" : "ollama";
    return false;
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
    commitSettings();
    return true;
  }
  return false;
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
      // Persist once per completed turn — the one moment the session id (worker
      // is done writing it) and the transcript are both final. Crash-safe by
      // construction: whatever the store holds is a whole conversation.
      saveConversationLocked();
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
        {kKeyBackend, &state_.pending_backend},          {kKeyOllamaUrl, &state_.pending_ollama_url},
        {kKeyOllamaModel, &state_.pending_ollama_model}, {kKeyClaudeModel, &state_.pending_claude_model},
        {kKeyClaudeCli, &state_.pending_claude_cli},
    };
    for (const auto& [key, value] : staged) {
      if (value->has_value()) {
        store.setString(key, **value);
        value->reset();
      }
    }
    backend = store.getString(kKeyBackend, "ollama");

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
