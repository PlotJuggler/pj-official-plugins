#pragma once

/// @file source_connection.hpp
/// @brief The connection half of a websocket streaming source, shared so every such source
///  connects, fails and reconnects the same way: it adopts the dialog's open connection or dials
///  a fresh one, enforces the connect deadline, latches the first stream-stopping failure and,
///  when configured, re-dials a lost connection on a fixed delay. Frames reach the owner through
///  Handlers in arrival order. Everything runs on the poll thread (start() and adopt() on the
///  thread that starts the source, before polling) and is driven by poll(); nothing waits.

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <pj_websocket/url.hpp>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <utility>
#include <vector>

namespace PJ::WebSocket {

struct SourceConnectionOptions {
  /// Names the server in messages: "failed to connect to <name> at <url>", "<name> stream at";
  /// <url>, also where a transport error quotes it, is shown cleaned (cleanUrl).
  std::string name;
  /// How long one attempt may take before it fails with "timed out".
  std::chrono::milliseconds connect{5000};
  /// Unset: losing the connection stops the stream. Set: a lost connection is re-dialled after
  /// this delay, and again after each failed attempt; only the first connection of a start
  /// failing stops the stream.
  std::optional<std::chrono::milliseconds> reconnect;
  size_t inbound_queue_bytes = EventQueue::kDefaultMaxBytes;
};

class SourceConnection {
 public:
  using Level = PJ::DataSourceMessageLevel;

  struct Handlers {
    /// The socket opened, or reopened after a loss (`reconnected`, a fresh server session).
    std::function<void(bool reconnected)> on_open;
    /// One text frame. Wire data can be wrong-typed and nlohmann accessors throw: an exception
    /// drops the frame with a warning, so a close queued behind it is still seen.
    std::function<void(const std::string&)> on_text;
    /// One binary frame, the queue's own copy, so the owner may keep it without copying.
    std::function<void(std::string&&)> on_binary;
    /// A message for the user: malformed frames, dropped data, a lost or restored connection.
    std::function<void(Level, const std::string&)> report;
  };

  /// The handlers usually capture the owning source, which cannot be copied or moved once it
  /// holds a SourceConnection.
  SourceConnection(SourceConnectionOptions options, Handlers handlers)
      : options_(std::move(options)), handlers_(std::move(handlers)) {}

  /// [onStart] Takes over the dialog's connection when it is open and returns true. The next
  /// poll() reports it through on_open(false), as for a fresh connection, and then delivers the
  /// frames the dialog did not drain. A reconnection dials the handoff's url and subprotocols.
  bool adopt(Handoff handoff) {
    if (!handoff.socket || !handoff.socket->isOpen()) {
      return false;
    }
    begin(std::move(handoff.url), std::move(handoff.subprotocols));
    socket_ = std::move(handoff.socket);
    events_ = std::move(handoff.events);
    phase_ = Phase::kConnecting;
    deadline_ = Clock::time_point::max();  // already open: opened() runs before any deadline check
    adopted_ = true;
    return true;
  }

  /// [onStart] Starts connecting to `url`; poll() applies the outcome. Fails only when the
  /// attempt cannot even be scheduled.
  [[nodiscard]] PJ::Status start(std::string url, std::vector<std::string> subprotocols = {}) {
    begin(std::move(url), std::move(subprotocols));
    phase_ = Phase::kConnecting;
    if (auto status = dial(); !status) {
      return PJ::unexpected(
          "failed to connect to " + options_.name + " at " + cleanUrl(url_) + ": " +
          withCleanUrl(status.error(), url_));
    }
    return PJ::okStatus();
  }

  /// [onPoll] Applies every queued event in arrival order, then the deadlines. Returns the
  /// first stream-stopping failure, latched until stop().
  [[nodiscard]] PJ::Status poll() {
    if (adopted_) {
      adopted_ = false;
      opened();  // before the frames queued behind the dialog's own kOpen
    }
    pump();
    const auto now = Clock::now();
    if ((phase_ == Phase::kConnecting || phase_ == Phase::kReconnecting) && now >= deadline_) {
      down("timed out");
    }
    if (phase_ == Phase::kLost && now >= deadline_) {
      phase_ = Phase::kReconnecting;
      if (auto status = dial(); !status) {
        down(status.error());
      }
    }
    if (phase_ == Phase::kFailed) {
      return PJ::unexpected(fatal_error_);
    }
    return PJ::okStatus();
  }

  /// Open, as far as the events applied so far tell: on_open has run and no close or error has
  /// been applied since.
  [[nodiscard]] bool isOpen() const {
    return phase_ == Phase::kOpen;
  }

  /// Sends a text frame when the socket is open; otherwise the frame is dropped.
  void sendText(std::string text) {
    if (socket_) {
      socket_->sendText(std::move(text));
    }
  }

  /// [poll thread] Stops the stream with `message`, e.g. on a protocol the owner cannot speak,
  /// unless it was already stopped: the first failure is the one poll() reports.
  void fail(std::string message) {
    if (phase_ == Phase::kFailed) {
      return;
    }
    if (socket_) {
      socket_->stop();
    }
    events_.reset();
    fatal_error_ = std::move(message);
    phase_ = Phase::kFailed;
  }

  /// [onStop] Closes the connection and forgets this start; late callbacks land in a queue
  /// nobody drains.
  void stop() {
    socket_.reset();  // stops it
    events_.reset();
    phase_ = Phase::kIdle;
    adopted_ = false;
    fatal_error_.clear();
    drop_warning_ = {};
  }

 private:
  using Clock = std::chrono::steady_clock;

  enum class Phase {
    kIdle,
    kConnecting,    ///< the first attempt of this start, until deadline_
    kOpen,          ///< on_open has run
    kLost,          ///< reconnect mode: waiting until deadline_ to dial again
    kReconnecting,  ///< reconnect mode: an attempt after a loss, until deadline_
    kFailed,        ///< stopped by fatal_error_ until stop()
  };

  void begin(std::string url, std::vector<std::string> subprotocols) {
    stop();
    url_ = std::move(url);
    subprotocols_ = std::move(subprotocols);
    drop_label_ = options_.name + " stream at " + cleanUrl(url_);
  }

  /// One attempt on a fresh socket and queue, so a previous socket's late events are never seen.
  PJ::Status dial() {
    socket_.reset();
    events_ = std::make_shared<EventQueue>(options_.inbound_queue_bytes);
    drop_warning_ = {};  // the new queue counts its drops from zero
    socket_ = std::make_unique<Client>();
    deadline_ = Clock::now() + options_.connect;
    return socket_->start(url_, events_, subprotocols_);
  }

  void pump() {
    if (!events_) {
      return;
    }
    const EventSink queue = events_;  // its drops are still reported if a close drops events_
    // Pop each event as it is applied, so a long batch frees its frames as it goes. While this
    // batch (up to the queue's cap) is applied, the transport refills the queue up to the cap
    // again: the inbound bound is about twice inbound_queue_bytes.
    auto pending = queue->drain();
    // A close, an error or fail() drops the queue: this connection's later events are moot.
    while (!pending.empty() && events_) {
      Event event = std::move(pending.front());
      pending.pop_front();
      switch (event.type) {
        case Event::Type::kOpen:
          if ((phase_ == Phase::kConnecting || phase_ == Phase::kReconnecting) && event.arrived >= deadline_) {
            down("timed out");  // opened after the deadline, merely before this poll
          } else {
            opened();
          }
          break;
        case Event::Type::kText:
          try {
            handlers_.on_text(event.payload);
          } catch (const std::exception& e) {
            handlers_.report(Level::kWarning, std::string("Dropped malformed control frame: ") + e.what());
          }
          break;
        case Event::Type::kBinary:
          handlers_.on_binary(std::move(event.payload));
          break;
        case Event::Type::kClosed:
          down(event.payload.empty() ? "closed by the server" : event.payload);
          break;
        case Event::Type::kError:
          down(event.payload);
          break;
      }
    }
    if (auto warning = drop_warning_.check(*queue, drop_label_)) {
      handlers_.report(Level::kWarning, *warning);
    }
  }

  void opened() {
    if (phase_ == Phase::kConnecting) {
      phase_ = Phase::kOpen;
      handlers_.on_open(false);
    } else if (phase_ == Phase::kReconnecting) {
      phase_ = Phase::kOpen;
      handlers_.on_open(true);
      handlers_.report(Level::kInfo, "Reconnected to " + options_.name);
    }
    // A kOpen the dialog left queued for an adopted connection is not news.
  }

  /// A close, an error or a missed deadline.
  void down(const std::string& reason) {
    const bool was_open = phase_ == Phase::kOpen;
    if (phase_ == Phase::kConnecting || !options_.reconnect) {
      fail(
          (was_open ? "lost connection to " : "failed to connect to ") + options_.name + " at " + cleanUrl(url_) +
          (reason.empty() ? "" : ": " + withCleanUrl(reason, url_)));
      return;
    }
    if (was_open) {
      handlers_.report(Level::kWarning, options_.name + " connection lost");
    }
    socket_->stop();  // dial() replaces it
    events_.reset();
    phase_ = Phase::kLost;
    deadline_ = Clock::now() + *options_.reconnect;
  }

  const SourceConnectionOptions options_;
  const Handlers handlers_;

  std::string url_;
  std::vector<std::string> subprotocols_;
  std::string drop_label_;
  std::unique_ptr<Client> socket_;
  EventSink events_;  ///< fresh per attempt
  Phase phase_ = Phase::kIdle;
  bool adopted_ = false;  ///< adopt() succeeded; the next poll() reports the open
  Clock::time_point deadline_{};
  std::string fatal_error_;
  DropWarning drop_warning_;
};

}  // namespace PJ::WebSocket
