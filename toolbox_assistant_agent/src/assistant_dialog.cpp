// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "assistant_dialog.hpp"

#include <cstdlib>
#include <utility>

#include "assistant_panel_manifest.hpp"
#include "assistant_panel_ui.hpp"
#include "assistant_settings_ui.hpp"
#include "claude_backend.hpp"
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

void AssistantDialog::rebuildBackend() {
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
          store.getString(kKeyOllamaUrl, kDefaultOllamaUrl), store.getString(kKeyOllamaModel, ""));
    } else if (choice == "claude") {
      backend_ = std::make_shared<ClaudeBackend>(
          store.getString(kKeyClaudeCli, "claude"), store.getString(kKeyClaudeModel, ""));
    } else {
      // Unset choice -> the harmless echo backend until the user picks one.
      backend_ = std::make_shared<EchoBackend>();
    }
  }
  std::lock_guard<std::mutex> lock(state_.mu);
  state_.backend_name = backend_->name();
  state_.header_dirty = true;
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

void AssistantDialog::setPlaybackProvider(std::function<PJ::sdk::PlaybackHostView()> provider) {
  playback_provider_ = std::move(provider);
}

void AssistantDialog::setViewportProvider(std::function<PJ::sdk::ViewportHostView()> provider) {
  viewport_provider_ = std::move(provider);
}

void AssistantDialog::setSettings(PJ::sdk::SettingsView settings) {
  settings_ = settings;
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
  if (playback_provider_) {
    ctx.playback = playback_provider_();
  }
  if (viewport_provider_) {
    ctx.viewport = viewport_provider_();
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
    wd.setLabel("statusLabel", state_.session.statusText());
    wd.setEnabled("inputEdit", !busy);
    wd.setEnabled("sendButton", !busy);
    wd.setEnabled("cancelButton", busy);
    // A mid-turn backend rebuild would strand the in-flight turn on the old
    // backend (kept alive by the worker's reference) — just don't offer it.
    wd.setEnabled("settingsButton", !busy);
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
    wd.setText("claudeModelEdit", store.getString(kKeyClaudeModel, ""));
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
  return tools_ran > 0 || !batch.empty();
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
  postCommand([this, text, backend = backend_]() {
    LlmBackend::EventSink sink = [this](BackendEvent ev) { postEvent([this, ev]() { applyBackendEvent(ev); }); };
    TurnTools tools;
    tools.registry = &registry_;
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
      state_.session.addAssistant(ev.text);
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
    case BackendEvent::Kind::TurnComplete:
      state_.session.setState(TurnState::Idle);
      state_.controls_dirty = true;
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
