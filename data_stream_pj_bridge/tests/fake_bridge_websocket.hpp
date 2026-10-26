#pragma once

/// @file fake_bridge_websocket.hpp
/// @brief Test backend for BridgeWebSocket (linked instead of the native or browser backend):
///  the test plays the server by reading what a socket sent and pushing events into its sink.

#include <memory>
#include <string>
#include <vector>

#include "pj_bridge_websocket.hpp"

namespace PJ::BridgeProtocol::testing {

/// What one fake socket saw. Single-threaded: the test pushes events on its own thread.
struct FakeSocket {
  std::string url;
  EventSink sink;
  std::vector<std::string> sent;
  bool open = false;
  bool stopped = false;

  /// The server accepted the connection.
  void acceptConnection() {
    open = true;
    sink->push({BridgeWebSocketEvent::Type::kOpen, {}});
  }

  void deliverText(std::string text) const {
    sink->push({BridgeWebSocketEvent::Type::kText, std::move(text)});
  }

  void deliverBinary(std::string bytes) const {
    sink->push({BridgeWebSocketEvent::Type::kBinary, std::move(bytes)});
  }

  void closeFromServer(std::string reason) {
    open = false;
    sink->push({BridgeWebSocketEvent::Type::kClosed, std::move(reason)});
  }
};

/// Every fake socket started since the test began, oldest first. Unlike the real backends, a
/// stopped fake keeps its sink open, so tests prove the source isolates sessions on its own.
std::vector<std::shared_ptr<FakeSocket>>& fakeSockets();

}  // namespace PJ::BridgeProtocol::testing
