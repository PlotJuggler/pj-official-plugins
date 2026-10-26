#pragma once

/// @file pj_bridge_websocket.hpp
/// @brief The PlotJuggler Bridge plugin's WebSocket transport: one non-blocking contract with a
///  backend per platform, chosen by which source file the build compiles.
///
///  - pj_bridge_websocket_native.cpp: IXWebSocket; events arrive on IXWebSocket's own thread.
///  - pj_bridge_websocket_wasm.cpp: <emscripten/websocket.h>; the browser socket, its callbacks
///    and every send/close/delete run on the main runtime thread.
///
///  Either way, start() only begins connecting, and every outcome reaches the owner as an owned
///  event in a BridgeEventQueue that the owner drains from its own (poll or dialog-tick) thread.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <pj_base/expected.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ::BridgeProtocol {

/// One transport event. Owns its bytes: the transport's own buffer is gone by the time the owner
/// drains the event.
struct BridgeWebSocketEvent {
  enum class Type { kOpen, kText, kBinary, kClosed, kError };

  Type type = Type::kText;
  /// Frame bytes for kText/kBinary; a human-readable reason (possibly empty) for kClosed/kError.
  std::string payload;
};

/// Bounded FIFO from a transport's callback thread to the thread that owns the connection.
///
/// Overflow policy: pending data frames (kBinary) are capped at `max_bytes` of payload plus a
/// fixed per-frame overhead. Past the cap the OLDEST pending data frames are dropped and counted
/// in dropped(); the newest frame is always kept. Control events (open, text, close, error) are
/// never dropped and do not count against the cap: they are small and carry protocol state. A
/// PJRB data frame is self-contained, so a dropped one leaves a gap in the plots, never a broken
/// stream.
///
/// close() discards pending events and turns every later push() into a no-op: that is how a
/// transport drops callbacks that land after stop().
class BridgeEventQueue {
 public:
#ifdef __EMSCRIPTEN__
  /// The wasm32 heap is shared with the whole app and cannot exceed 4 GiB.
  static constexpr size_t kDefaultMaxBytes = size_t{64} * 1024 * 1024;
#else
  static constexpr size_t kDefaultMaxBytes = size_t{512} * 1024 * 1024;
#endif

  /// Data frames discarded by the overflow policy since construction.
  struct DropCount {
    uint64_t frames = 0;
    uint64_t bytes = 0;
  };

  explicit BridgeEventQueue(size_t max_bytes = kDefaultMaxBytes) : max_bytes_(max_bytes) {}

  /// [transport thread]
  void push(BridgeWebSocketEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    const bool data = event.type == BridgeWebSocketEvent::Type::kBinary;
    if (data) {
      queued_data_bytes_ += cost(event);
    }
    events_.push_back(std::move(event));
    if (data) {
      dropOldestDataPastBudget();
    }
  }

  /// [owner thread] Everything pending, in arrival order.
  [[nodiscard]] std::deque<BridgeWebSocketEvent> drain() {
    std::deque<BridgeWebSocketEvent> pending;
    std::lock_guard<std::mutex> lock(mutex_);
    pending.swap(events_);
    queued_data_bytes_ = 0;
    return pending;
  }

  void close() {
    std::deque<BridgeWebSocketEvent> discarded;  // freed after the lock: it may hold the whole cap
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    discarded.swap(events_);
    queued_data_bytes_ = 0;
  }

  [[nodiscard]] DropCount dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  /// Charged per data frame so a flood of tiny frames is bounded too.
  static constexpr size_t kFrameOverheadBytes = 64;

  [[nodiscard]] static size_t cost(const BridgeWebSocketEvent& event) {
    return event.payload.size() + kFrameOverheadBytes;
  }

  /// Called with the lock held, right after a data frame was appended.
  void dropOldestDataPastBudget() {
    auto candidate = events_.begin();
    while (queued_data_bytes_ > max_bytes_) {
      const auto newest = std::prev(events_.end());
      candidate = std::find_if(candidate, newest, [](const BridgeWebSocketEvent& event) {
        return event.type == BridgeWebSocketEvent::Type::kBinary;
      });
      if (candidate == newest) {
        return;  // only the newest frame is left: keep it even when it alone exceeds the cap
      }
      queued_data_bytes_ -= cost(*candidate);
      ++dropped_.frames;
      dropped_.bytes += candidate->payload.size();
      candidate = events_.erase(candidate);
    }
  }

  const size_t max_bytes_;
  mutable std::mutex mutex_;
  std::deque<BridgeWebSocketEvent> events_;
  size_t queued_data_bytes_ = 0;
  bool closed_ = false;
  DropCount dropped_;
};

/// Where a BridgeWebSocket delivers its events. Shared, so callbacks still queued on another
/// thread can outlive the owner's stop() safely.
using EventSink = std::shared_ptr<BridgeEventQueue>;

/// True for "ws://" or "wss://" followed by a non-empty host; any port and path are kept as is.
[[nodiscard]] inline bool isWebSocketUrl(std::string_view url) {
  const std::string_view host = url.starts_with("wss://")  ? url.substr(6)
                                : url.starts_with("ws://") ? url.substr(5)
                                                           : std::string_view{};
  return !host.empty() && host.front() != '/';
}

/// [native backend] The first of `candidates` that exists, else "SYSTEM" (IXWebSocket's default,
/// which its mbedTLS build can read only on Windows): the CA bundle wss:// is verified against.
[[nodiscard]] std::string firstExistingCaFile(const std::vector<std::string>& candidates);

/// Plugin-local WebSocket client with the same contract on every backend:
///  - start() returns before the connection completes; open, frames, close and errors arrive
///    later in the sink. Call it once per object.
///  - sendText(), isOpen() and stop() may be called from any thread; none of them waits for the
///    browser main thread, which may itself be joining the caller.
///  - stop() is idempotent and closes the sink first, so callbacks that land late are dropped.
class BridgeWebSocket {
 public:
  /// Backend-defined connection state, shared with callbacks that may outlive stop().
  struct State;

  BridgeWebSocket() = default;
  ~BridgeWebSocket() {
    stop();
  }
  BridgeWebSocket(const BridgeWebSocket&) = delete;
  BridgeWebSocket& operator=(const BridgeWebSocket&) = delete;

  /// Begins connecting to `url` (ws:// or wss://, any path). Fails only when the attempt cannot
  /// even be scheduled; a refused or unreachable server is reported as a kError/kClosed event.
  [[nodiscard]] PJ::Status start(std::string url, EventSink sink);

  /// Sends a text frame when open; otherwise the frame is dropped.
  void sendText(std::string text);

  /// As far as the calling thread can tell; may run ahead of the kOpen/kClosed events not yet drained.
  [[nodiscard]] bool isOpen() const;

  /// The native backend joins IXWebSocket's own thread; the browser backend queues close and
  /// delete on the main runtime thread and returns.
  void stop();

 private:
  std::shared_ptr<State> state_;
};

}  // namespace PJ::BridgeProtocol
