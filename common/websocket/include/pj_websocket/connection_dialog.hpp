#pragma once

/// @file connection_dialog.hpp
/// @brief The connection half of a websocket streamer's dialog, shared so every such dialog looks
///  and behaves the same:
///   - Endpoint: the "Address or URL" / "Port" fields and the read-only transport indicator
///     (widgets lineEditAddress, lineEditPort, comboBoxTransport), and their config keys.
///   - DialogConnection: Connect / Cancel / Disconnect, one deadline per attempt, retries with a
///     doubling capped backoff until the user cancels, the status line (labelConnectionStatus),
///     and the handoff of the live connection to the source on OK.
///  Everything runs on the dialog thread and is polled from onTick(); nothing waits.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/endpoint.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ::WebSocket {

/// Where a dialog connects: a full ws:// / wss:// URL typed into the address field, else
/// ws://address:port.
struct Endpoint {
  std::string address;
  int port = 0;
  std::string url;  ///< Non-empty: a full ws:// or wss:// URL that replaces address/port.

  /// The URL to connect to.
  [[nodiscard]] std::string str() const {
    return url.empty() ? PJ::sdk::composeEndpoint("ws", address, static_cast<uint16_t>(port)) : url;
  }

  /// Fills the connection widgets; they are editable only while `editable`.
  void toWidgets(PJ::WidgetData& wd, bool editable) const {
    wd.setText("lineEditAddress", url.empty() ? address : url);
    wd.setText("lineEditPort", std::to_string(port));
    wd.setEnabled("lineEditAddress", editable);
    wd.setEnabled("lineEditPort", editable && url.empty());
    wd.setCurrentIndex("comboBoxTransport", url.starts_with("wss://") ? 1 : 0);
  }

  /// Applies an edit of `widget_name`. Returns whether it was a connection field, and sets
  /// `refresh` when the UI must follow (the port field and the scheme follow the URL mode).
  bool onTextChanged(std::string_view widget_name, std::string_view text, bool& refresh) {
    if (widget_name == "lineEditAddress") {
      const bool was_url = !url.empty();
      if (isWebSocketUrl(text)) {
        url = std::string(text);
      } else {
        url.clear();
        address = std::string(text);
      }
      refresh = was_url != !url.empty();
      return true;
    }
    if (widget_name == "lineEditPort") {
      if (const auto parsed = PJ::sdk::parsePort(text)) {
        port = *parsed;
      }
      refresh = false;
      return true;
    }
    return false;
  }

  /// Writes "address", "port" and, when set, "url".
  void save(nlohmann::json& cfg) const {
    cfg["address"] = address;
    cfg["port"] = port;
    if (!url.empty()) {
      cfg["url"] = url;
    }
  }

  /// Reads what save() wrote, keeping `default_address` / `default_port` for absent keys. A
  /// present "url" must be a ws:// or wss:// string and wins over address/port, which are then
  /// not read at all. Throws nlohmann::json::exception for a wrong-typed address or port.
  [[nodiscard]] bool load(const nlohmann::json& cfg, const std::string& default_address, int default_port) {
    std::string loaded_url;
    if (const auto it = cfg.find("url"); it != cfg.end()) {
      if (!it->is_string()) {
        return false;
      }
      loaded_url = it->get<std::string>();
      if (!isWebSocketUrl(loaded_url)) {
        return false;
      }
    }
    if (loaded_url.empty()) {
      address = cfg.value("address", default_address);
      port = cfg.value("port", default_port);
    }
    url = std::move(loaded_url);
    return true;
  }
};

/// Deadlines and limits a dialog connection enforces by polling; tests shorten them.
struct DialogConnectionOptions {
  /// Per connection attempt.
  std::chrono::milliseconds connect{5000};
  /// Retry backoff: first delay, doubled after each failure up to retry_max.
  std::chrono::milliseconds retry_first{1000};
  std::chrono::milliseconds retry_max{10000};
  size_t inbound_queue_bytes = EventQueue::kDefaultMaxBytes;
};

/// A live dialog connection handed to the source on OK: the socket and the queue its events
/// land in, events not yet drained included.
struct Handoff {
  std::unique_ptr<Client> socket;
  EventSink events;
};

/// A dialog's own connection. Every transition happens on the dialog thread, driven by the
/// dialog's onClicked() and onTick(); attempts, deadlines and retries are polled, never waited on.
class DialogConnection {
 public:
  enum class State {
    kIdle,        ///< not connecting (initially, after cancel/disconnect, after the handoff)
    kConnecting,  ///< one attempt in flight until its deadline
    kRetrying,    ///< the last attempt failed; the next starts when its delay ends
    kConnected,
  };

  /// What tick() reports back to the dialog.
  struct Handlers {
    /// The socket opened; the dialog may send its first requests.
    std::function<void()> on_open;
    /// One text frame. Wire data can be wrong-typed and nlohmann accessors throw: an exception
    /// drops the frame, so a close queued behind it is still seen.
    std::function<void(const std::string&)> on_text;
    /// The connection or attempt was dropped; the dialog forgets what it discovered.
    std::function<void()> on_lost;
  };

  explicit DialogConnection(DialogConnectionOptions options = {}) : options_(options) {}

  /// [idle] Starts connecting to `url`, retrying with a fresh backoff until disconnect().
  void connect(std::string url, std::vector<std::string> subprotocols = {}) {
    url_ = std::move(url);
    subprotocols_ = std::move(subprotocols);
    retry_delay_ = options_.retry_first;
    attempt();
  }

  /// Drops the current attempt or connection; back to idle.
  void disconnect() {
    socket_.reset();  // stops it
    events_.reset();
    state_ = State::kIdle;
  }

  /// Hands the connection to the caller (the source) and goes idle. Its socket is null when the
  /// dialog never connected.
  [[nodiscard]] Handoff take() {
    state_ = State::kIdle;
    return {std::move(socket_), std::move(events_)};
  }

  /// Applies queued events, the attempt deadline and the retry timer. True when the UI must refresh.
  bool tick(const Handlers& handlers) {
    if (state_ == State::kIdle) {
      return false;
    }
    bool changed = events_ && pumpEvents(handlers);
    if (state_ == State::kConnecting && Clock::now() >= attempt_deadline_) {
      scheduleRetry("timed out", handlers);
      changed = true;
    } else if (state_ == State::kRetrying) {
      if (Clock::now() >= next_attempt_) {
        attempt();
        changed = true;
      } else if (const int seconds = retrySecondsLeft(); seconds != shown_retry_seconds_) {
        shown_retry_seconds_ = seconds;  // the countdown in the status line moved
        changed = true;
      }
    }
    return changed;
  }

  /// Sends a text frame when connected; otherwise it is dropped.
  void sendText(std::string text) {
    if (socket_ && socket_->isOpen()) {
      socket_->sendText(std::move(text));
    }
  }

  [[nodiscard]] State state() const {
    return state_;
  }
  [[nodiscard]] bool idle() const {
    return state_ == State::kIdle;
  }
  [[nodiscard]] bool connected() const {
    return state_ == State::kConnected;
  }

  /// Fills the connect button and the status line.
  void toWidgets(PJ::WidgetData& wd) const {
    // Pressing the button while connecting or retrying cancels, and while connected disconnects.
    wd.setButtonText("buttonConnect", connected() ? "Connected" : (idle() ? "Connect" : "Cancel"));
    wd.setChecked("buttonConnect", !idle());
    wd.setLabel("labelConnectionStatus", status());
  }

  /// Connecting / retrying in N s with the last error / connected; empty when idle.
  [[nodiscard]] std::string status() const {
    switch (state_) {
      case State::kIdle:
        return {};
      case State::kConnecting:
        return "Connecting to " + url_ + "...";
      case State::kRetrying:
        return "Retrying in " + std::to_string(retrySecondsLeft()) + " s: " + last_error_;
      case State::kConnected:
        return "Connected to " + url_;
    }
    return {};
  }

 private:
  using Clock = std::chrono::steady_clock;

  /// Starts one connection attempt with its own deadline.
  void attempt() {
    events_ = std::make_shared<EventQueue>(options_.inbound_queue_bytes);
    socket_ = std::make_unique<Client>();
    state_ = State::kConnecting;
    attempt_deadline_ = Clock::now() + options_.connect;
    if (auto status = socket_->start(url_, events_, subprotocols_); !status) {
      scheduleRetry(status.error(), {});
    }
  }

  /// Drops the current attempt or connection and tries again after a doubling, capped delay.
  void scheduleRetry(const std::string& reason, const Handlers& handlers) {
    socket_.reset();  // stops it
    events_.reset();
    if (handlers.on_lost) {
      handlers.on_lost();
    }
    last_error_ = reason.empty() ? "connection closed" : reason;
    state_ = State::kRetrying;
    next_attempt_ = Clock::now() + retry_delay_;
    retry_delay_ = std::min(retry_delay_ * 2, options_.retry_max);
    shown_retry_seconds_ = retrySecondsLeft();
  }

  [[nodiscard]] int retrySecondsLeft() const {
    const auto left = std::chrono::ceil<std::chrono::seconds>(next_attempt_ - Clock::now()).count();
    return static_cast<int>(std::max<decltype(left)>(left, 1));
  }

  /// Data frames are ignored: a dialog subscribes to nothing. The queue never drops control
  /// events, so a close is always seen.
  bool pumpEvents(const Handlers& handlers) {
    bool changed = false;
    for (auto& event : events_->drain()) {
      switch (event.type) {
        case Event::Type::kOpen:
          state_ = State::kConnected;
          retry_delay_ = options_.retry_first;
          last_error_.clear();
          if (handlers.on_open) {
            handlers.on_open();
          }
          changed = true;
          break;
        case Event::Type::kText:
          try {
            if (handlers.on_text) {
              handlers.on_text(event.payload);
            }
          } catch (const nlohmann::json::exception&) {}
          changed = true;
          break;
        case Event::Type::kBinary:
          break;
        case Event::Type::kClosed:
        case Event::Type::kError:
          scheduleRetry(event.payload, handlers);  // releases this connection's queue; later events are moot
          return true;
      }
    }
    return changed;
  }

  DialogConnectionOptions options_;
  State state_ = State::kIdle;
  std::string url_;
  std::vector<std::string> subprotocols_;
  std::unique_ptr<Client> socket_;
  EventSink events_;
  Clock::time_point attempt_deadline_{};
  Clock::time_point next_attempt_{};
  std::chrono::milliseconds retry_delay_{};
  std::string last_error_;       ///< why the last attempt failed, shown while retrying
  int shown_retry_seconds_ = 0;  ///< the countdown last rendered, so tick() refreshes once a second
};

}  // namespace PJ::WebSocket
