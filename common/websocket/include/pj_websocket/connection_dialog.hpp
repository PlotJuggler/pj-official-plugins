#pragma once

/// @file connection_dialog.hpp
/// @brief The connection half of a websocket streamer's dialog, shared so every such dialog looks
///  and behaves the same:
///   - Endpoint: the one-line server URL (widget lineEditUrl), its config key and the last URL
///     remembered through the host's optional settings store.
///   - DialogConnection: Connect / Cancel / Disconnect, one deadline per attempt, retries with a
///     doubling capped backoff until the user cancels, the status line (labelConnectionStatus),
///     and the handoff of the live connection to the source on OK.
///  Everything runs on the dialog thread and is polled from onTick(); nothing waits.
///
///  The dialog's .ui must provide lineEditUrl, buttonConnect and labelConnectionStatus;
///  DialogConnection::toWidgets() fills all three from its state and the Endpoint.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_plugins/sdk/endpoint.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_websocket/url.hpp>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PJ::WebSocket {

/// Settings key (service pj.settings.v1, namespaced per plugin by the host) of the last URL a
/// dialog connected to, stored cleaned of credentials (cleanUrl). The `durable/` prefix is the
/// host's convention for a key that survives a browser reload (values up to 2048 characters);
/// on the desktop it is an ordinary key.
inline constexpr std::string_view kLastUrlSettingsKey = "durable/last_url";

/// Where a dialog connects: the one URL line (widget lineEditUrl), its config key and the
/// remembered last URL.
class Endpoint {
 public:
  /// `default_host` and `default_port` make the initial URL and complete a bare host.
  Endpoint(std::string_view default_host, uint16_t default_port)
      : default_host_(default_host),
        default_port_(default_port),
        text_(PJ::sdk::composeEndpoint("ws", default_host, default_port)),
        url_(normalizeUrl(text_, default_port)) {}

  /// The URL to dial (normalizeUrl of the line), or why the line is not one.
  [[nodiscard]] const Expected<std::string>& url() const {
    return url_;
  }

  /// The URL line as typed.
  [[nodiscard]] const std::string& text() const {
    return text_;
  }

  /// url(), or the raw text when it is not a URL (dialing it then fails with a clear error).
  [[nodiscard]] std::string str() const {
    return url_ ? *url_ : text_;
  }

  /// Applies an edit of `widget_name`: nullopt when it is not the URL line, else whether the UI
  /// must refresh (Connect and the status line follow the URL's validity and error).
  std::optional<bool> onTextChanged(std::string_view widget_name, std::string_view text) {
    if (widget_name != "lineEditUrl") {
      return std::nullopt;
    }
    const std::string before = error();
    setText(std::string(text));
    return error() != before;
  }

  /// Writes "url": the URL that is dialed (normalized), or the text as typed when it is not one.
  void save(nlohmann::json& cfg) const {
    cfg["url"] = str();
  }

  /// Reads "url", else the legacy "address" / "port", composed into ws://address:port (the
  /// default host or port for the missing one). Either way the URL must pass normalizeUrl. A
  /// config with none of them leaves the URL as it is. False for a "url" that is not a string,
  /// or a URL normalizeUrl refuses. Throws nlohmann::json::exception for a wrong-typed address
  /// or port.
  [[nodiscard]] bool load(const nlohmann::json& cfg) {
    std::string text;
    if (const auto it = cfg.find("url"); it != cfg.end()) {
      if (!it->is_string()) {
        return false;
      }
      text = it->get<std::string>();
    } else if (cfg.contains("address") || cfg.contains("port")) {
      std::string address = cfg.value("address", std::string{});
      text = PJ::sdk::composeEndpoint(
          "ws", address.empty() ? default_host_ : address, std::to_string(cfg.value("port", int64_t{default_port_})));
    } else {
      return true;
    }
    if (!normalizeUrl(text, default_port_)) {
      return false;
    }
    setText(std::move(text));
    from_config_ = true;
    return true;
  }

  /// Binds the host's optional settings store (pj.settings.v1; an unbound view when the host has
  /// none) and, unless a config already named the URL, starts from the remembered one. Call from
  /// the source's bind(): the view outlives the registry it came from.
  void bind(PJ::sdk::SettingsView settings) {
    settings_ = settings;
    if (!settings_.valid() || from_config_) {
      return;
    }
    if (auto stored = settings_.value(kLastUrlSettingsKey); stored && !stored->isNull()) {
      std::string last = stored->toString();
      if (normalizeUrl(last, default_port_)) {
        setText(std::move(last));
      }
    }
  }

  /// Stores `dialed`, cleaned of credentials (cleanUrl), as the URL to start from next time.
  /// Call once a connection to it opened. No-op without a store.
  void remember(const std::string& dialed) const {
    if (settings_.valid()) {
      (void)settings_.setValue(kLastUrlSettingsKey, cleanUrl(dialed));
    }
  }

 private:
  void setText(std::string text) {
    text_ = std::move(text);
    url_ = normalizeUrl(text_, default_port_);
  }

  /// Why the text is not a URL; empty when it is one.
  [[nodiscard]] std::string error() const {
    return url_ ? std::string{} : url_.error();
  }

  std::string default_host_;
  uint16_t default_port_;
  std::string text_;
  Expected<std::string> url_;  ///< normalizeUrl(text_), kept in step by setText()
  bool from_config_ = false;   ///< a loaded config named the URL: it wins over the remembered one
  PJ::sdk::SettingsView settings_;
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

/// A dialog's own connection. Every transition happens on the dialog thread, driven by the
/// dialog's onClicked() and onTick(); attempts, deadlines and retries are polled, never waited on.
class DialogConnection {
 public:
  /// What the connection reports back to the dialog, from tick().
  struct Handlers {
    /// The socket to `url` opened (the URL connect() dialed, whatever the URL line says now);
    /// the dialog may send its first requests.
    std::function<void(const std::string& url)> on_open;
    /// One text frame. Wire data can be wrong-typed and nlohmann accessors throw: an exception
    /// drops the frame, so a close queued behind it is still seen.
    std::function<void(const std::string&)> on_text;
    /// The connection or attempt was dropped; the dialog forgets what it discovered.
    std::function<void()> on_lost;
  };

  /// The handlers usually capture the owning dialog, which cannot be copied or moved once it
  /// holds a DialogConnection.
  DialogConnection(DialogConnectionOptions options, Handlers handlers)
      : options_(options), handlers_(std::move(handlers)) {}

  /// [idle] Starts connecting to `url`, retrying with a fresh backoff until disconnect(). Every
  /// retry dials this same `url`: a config loaded meanwhile applies from the next connect().
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
    return {std::move(socket_), std::move(events_), url_, subprotocols_};
  }

  /// Applies queued events, the attempt deadline and the retry timer. True when the UI must refresh.
  bool tick() {
    if (state_ == State::kIdle) {
      return false;
    }
    bool changed = events_ && pumpEvents();
    if (state_ == State::kConnecting && Clock::now() >= attempt_deadline_) {
      scheduleRetry("timed out");
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
    if (socket_) {
      socket_->sendText(std::move(text));
    }
  }

  [[nodiscard]] bool idle() const {
    return state_ == State::kIdle;
  }
  [[nodiscard]] bool connected() const {
    return state_ == State::kConnected;
  }

  /// Fills the URL line (editable only while idle), the connect button and the status line.
  /// While idle with an invalid URL, Connect is disabled and the status line says why.
  void toWidgets(PJ::WidgetData& wd, const Endpoint& endpoint) const {
    wd.setText("lineEditUrl", endpoint.text());
    wd.setEnabled("lineEditUrl", idle());
    // Pressing the button while connecting or retrying cancels, and while connected disconnects.
    wd.setButtonText("buttonConnect", connected() ? "Connected" : (idle() ? "Connect" : "Cancel"));
    wd.setChecked("buttonConnect", !idle());
    const auto& url = endpoint.url();
    wd.setEnabled("buttonConnect", !idle() || url.has_value());
    wd.setLabel("labelConnectionStatus", idle() && !url ? url.error() : status());
  }

 private:
  using Clock = std::chrono::steady_clock;

  enum class State {
    kIdle,        ///< not connecting (initially, after cancel/disconnect, after the handoff)
    kConnecting,  ///< one attempt in flight until its deadline
    kRetrying,    ///< the last attempt failed; the next starts when its delay ends
    kConnected,
  };

  /// Connecting / retrying in N s with the last error / connected; empty when idle. The URL is
  /// shown cleaned (cleanUrl): no token on screen.
  [[nodiscard]] std::string status() const {
    switch (state_) {
      case State::kIdle:
        return {};
      case State::kConnecting:
        return "Connecting to " + cleanUrl(url_) + "...";
      case State::kRetrying:
        return "Retrying in " + std::to_string(retrySecondsLeft()) + " s: " + last_error_;
      case State::kConnected:
        return "Connected to " + cleanUrl(url_);
    }
    return {};
  }

  /// Starts one connection attempt with its own deadline.
  void attempt() {
    events_ = std::make_shared<EventQueue>(options_.inbound_queue_bytes);
    socket_ = std::make_unique<Client>();
    state_ = State::kConnecting;
    attempt_deadline_ = Clock::now() + options_.connect;
    if (auto status = socket_->start(url_, events_, subprotocols_); !status) {
      scheduleRetry(status.error());
    }
  }

  /// Drops the current attempt or connection and tries again after a doubling, capped delay.
  void scheduleRetry(const std::string& reason) {
    disconnect();
    if (handlers_.on_lost) {
      handlers_.on_lost();
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
  bool pumpEvents() {
    bool changed = false;
    for (auto& event : events_->drain()) {
      switch (event.type) {
        case Event::Type::kOpen:
          if (state_ == State::kConnecting && event.arrived >= attempt_deadline_) {
            scheduleRetry("timed out");  // opened after the deadline, merely before this tick
            return true;
          }
          state_ = State::kConnected;
          retry_delay_ = options_.retry_first;
          last_error_.clear();
          if (handlers_.on_open) {
            handlers_.on_open(url_);
          }
          changed = true;
          break;
        case Event::Type::kText:
          try {
            if (handlers_.on_text) {
              handlers_.on_text(event.payload);
            }
          } catch (const nlohmann::json::exception&) {}
          changed = true;
          break;
        case Event::Type::kBinary:
          break;
        case Event::Type::kClosed:
        case Event::Type::kError:
          scheduleRetry(event.payload);  // releases this connection's queue; later events are moot
          return true;
      }
    }
    return changed;
  }

  DialogConnectionOptions options_;
  Handlers handlers_;
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
