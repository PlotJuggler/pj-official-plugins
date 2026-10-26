// <emscripten/websocket.h> backend of BridgeWebSocket (WebAssembly builds with pthreads).
//
// Threading: the browser WebSocket lives on the main runtime thread, and so do its callbacks and
// every send/close/delete. The library's own entry points proxy SYNCHRONOUSLY from a worker,
// which would deadlock a worker stop() while the main thread is joining that worker, so this
// backend never calls them off the main thread: start(), sendText() and stop() post tasks to a
// private queue on the main thread and return at once. Callbacks copy each frame into the sink;
// the owner drains it from its own thread.

#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/websocket.h>

#include <atomic>
#include <cstring>

#include "pj_bridge_websocket.hpp"

EM_JS_DEPS(pj_bridge_websocket, "emscripten_websocket_new");

// new WebSocket(url) throws synchronously (SyntaxError for a malformed URL, SecurityError for ws://
// from an https page). Escaping into the proxy queue, that exception would wedge it, so it
// becomes a failed handle here.
EM_JS(int, pj_bridge_websocket_new_guarded, (const EmscriptenWebSocketCreateAttributes* attributes), {
  try {
    return _emscripten_websocket_new(attributes);
  } catch (error) {
    console.error('PlotJuggler Bridge: the browser refused to open the WebSocket:', error);
    return -1;
  }
});

namespace PJ::BridgeProtocol {

struct BridgeWebSocket::State {
  EventSink sink;
  std::string url;
  std::atomic<bool> open{false};
  std::atomic<bool> stop_requested{false};
  /// [main runtime thread] 0 until created, and again once deleted.
  EMSCRIPTEN_WEBSOCKET_T socket = 0;
  /// [main runtime thread] Holds the state while JS handlers can still fire with it as userData.
  std::shared_ptr<State> keep_alive;
};

namespace {

using StatePtr = std::shared_ptr<BridgeWebSocket::State>;
using Type = BridgeWebSocketEvent::Type;

/// Created once and never destroyed. Unlike the system queue, whose tasks may run inside any
/// wait or lock the main thread is in, these run only when it returns to the browser event loop.
em_proxying_queue* mainThreadQueue() {
  static em_proxying_queue* const queue = em_proxying_queue_create();
  return queue;
}

/// A unit of main-thread work; it keeps the state alive until it has run.
struct Task {
  void (*run)(Task& task);
  StatePtr state;
  std::string text;
};

/// Never waits: the task is queued, and the main thread runs it later on its own turn.
bool postToMainThread(void (*run)(Task&), StatePtr state, std::string text = {}) {
  auto* task = new Task{run, std::move(state), std::move(text)};
  const bool queued = emscripten_proxy_async(
      mainThreadQueue(), emscripten_main_runtime_thread_id(),
      [](void* arg) {
        const std::unique_ptr<Task> owned(static_cast<Task*>(arg));
        owned->run(*owned);
      },
      task);
  if (!queued) {
    delete task;
  }
  return queued;
}

BridgeWebSocket::State& stateOf(void* user_data) {
  return *static_cast<BridgeWebSocket::State*>(user_data);
}

bool onOpen(int, const EmscriptenWebSocketOpenEvent*, void* user_data) {
  auto& state = stateOf(user_data);
  if (!state.stop_requested) {
    state.open = true;
  }
  state.sink->push({Type::kOpen, {}});
  return true;
}

bool onMessage(int, const EmscriptenWebSocketMessageEvent* event, void* user_data) {
  // The runtime frees event->data when this returns, so the sink gets a copy. Text frames
  // arrive NUL-terminated.
  size_t size = event->numBytes;
  if (event->isText && size > 0) {
    --size;
  }
  stateOf(user_data).sink->push(
      {event->isText ? Type::kText : Type::kBinary, std::string(reinterpret_cast<const char*>(event->data), size)});
  return true;
}

bool onError(int, const EmscriptenWebSocketErrorEvent*, void* user_data) {
  // Browsers deliberately hide the cause (refused, TLS, DNS, mixed content...).
  stateOf(user_data).sink->push({Type::kError, "the browser reported a WebSocket error"});
  return true;
}

bool onClose(int, const EmscriptenWebSocketCloseEvent* event, void* user_data) {
  auto& state = stateOf(user_data);
  state.open = false;
  std::string reason(event->reason, strnlen(event->reason, sizeof(event->reason)));
  if (reason.empty()) {
    reason = "close code " + std::to_string(event->code);
  }
  state.sink->push({Type::kClosed, std::move(reason)});
  return true;
}

// --- Main runtime thread tasks ---

void createSocket(Task& task) {
  auto& state = *task.state;
  if (state.stop_requested) {
    return;
  }
  EmscriptenWebSocketCreateAttributes attributes;
  emscripten_websocket_init_create_attributes(&attributes);
  attributes.url = state.url.c_str();
  attributes.createOnMainThread = true;
  const EMSCRIPTEN_WEBSOCKET_T socket = pj_bridge_websocket_new_guarded(&attributes);
  if (socket <= 0) {
    state.sink->push({Type::kError, "the browser refused to open a WebSocket to " + state.url});
    return;
  }
  state.socket = socket;
  state.keep_alive = task.state;
  // On this thread, CALLING_THREAD is the main runtime thread the socket lives on.
  emscripten_websocket_set_onopen_callback(socket, &state, onOpen);
  emscripten_websocket_set_onmessage_callback(socket, &state, onMessage);
  emscripten_websocket_set_onerror_callback(socket, &state, onError);
  emscripten_websocket_set_onclose_callback(socket, &state, onClose);
}

void sendOnSocket(Task& task) {
  const auto& state = *task.state;
  if (state.socket > 0 && !state.stop_requested) {
    emscripten_websocket_send_utf8_text(state.socket, task.text.c_str());
  }
}

void closeSocket(Task& task) {
  auto& state = *task.state;
  if (state.socket <= 0) {
    return;
  }
  emscripten_websocket_close(state.socket, 1000, "client stopped");  // 1000: normal closure
  // Clears the JS handlers, so no callback can reach the state after this.
  emscripten_websocket_delete(state.socket);
  state.socket = 0;
  state.keep_alive.reset();
}

}  // namespace

PJ::Status BridgeWebSocket::start(std::string url, EventSink sink) {
  if (state_ || !sink) {
    return PJ::unexpected(std::string("BridgeWebSocket::start needs a fresh socket and an event sink"));
  }
  state_ = std::make_shared<State>();
  state_->sink = std::move(sink);
  state_->url = std::move(url);
  if (!postToMainThread(&createSocket, state_)) {
    return PJ::unexpected(std::string("could not schedule the WebSocket on the browser main thread"));
  }
  return PJ::okStatus();
}

void BridgeWebSocket::sendText(std::string text) {
  if (isOpen()) {
    postToMainThread(&sendOnSocket, state_, std::move(text));
  }
}

bool BridgeWebSocket::isOpen() const {
  // stop_requested is set first in stop(), so this reads false after stop() even when onOpen()
  // sets `open` on the main thread in between.
  return state_ && state_->open && !state_->stop_requested;
}

void BridgeWebSocket::stop() {
  if (!state_ || state_->stop_requested.exchange(true)) {
    return;
  }
  state_->open = false;
  state_->sink->close();
  // Known limit: if the post fails (the proxy queue cannot allocate), the socket is left open on
  // purpose and the closed sink drops its late events. Resetting keep_alive here instead would
  // free the state its handlers still use on the main thread.
  postToMainThread(&closeSocket, state_);
}

}  // namespace PJ::BridgeProtocol
