// IXWebSocket backend of BridgeWebSocket (desktop builds).

#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>

#include <filesystem>
#include <system_error>

#include "pj_bridge_websocket.hpp"

namespace PJ::BridgeProtocol {

std::string firstExistingCaFile(const std::vector<std::string>& candidates) {
  for (const auto& path : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
      return path;
    }
  }
  return "SYSTEM";
}

struct BridgeWebSocket::State {
  ix::WebSocket socket;
  EventSink sink;
};

PJ::Status BridgeWebSocket::start(std::string url, EventSink sink) {
  if (state_ || !sink) {
    return PJ::unexpected(std::string("BridgeWebSocket::start needs a fresh socket and an event sink"));
  }
  state_ = std::make_shared<State>();
  state_->sink = sink;
#ifndef _WIN32
  if (url.starts_with("wss://")) {
    // Debian/Ubuntu, Fedora/RHEL, openSUSE, macOS.
    ix::SocketTLSOptions tls;
    tls.caFile = firstExistingCaFile(
        {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
         "/etc/ssl/cert.pem"});
    state_->socket.setTLSOptions(tls);
  }
#endif
  state_->socket.setUrl(url);
  // The browser backend cannot reconnect either; a lost connection is reported, never retried.
  state_->socket.disableAutomaticReconnection();
  // A stop() that lands just as IXWebSocket starts its handshake loses the cancellation (the
  // handshake clears it), and the join then waits for this timeout. The default is 60 s; 5 s is
  // the source's own connect deadline. ponytail: bounds the freeze, a patched IXWebSocket ends it.
  state_->socket.setHandshakeTimeout(5);
  // Registered before start(): IXWebSocket invokes the callback for the very first event.
  // It captures only the queue, which stop() closes before joining the IXWebSocket thread.
  state_->socket.setOnMessageCallback([sink](const ix::WebSocketMessagePtr& message) {
    switch (message->type) {
      case ix::WebSocketMessageType::Open:
        sink->push({BridgeWebSocketEvent::Type::kOpen, {}});
        break;
      case ix::WebSocketMessageType::Message:
        sink->push(
            {message->binary ? BridgeWebSocketEvent::Type::kBinary : BridgeWebSocketEvent::Type::kText, message->str});
        break;
      case ix::WebSocketMessageType::Close:
        sink->push({BridgeWebSocketEvent::Type::kClosed, message->closeInfo.reason});
        break;
      case ix::WebSocketMessageType::Error:
        sink->push({BridgeWebSocketEvent::Type::kError, message->errorInfo.reason});
        break;
      default:
        break;  // ping, pong, fragment
    }
  });
  state_->socket.start();
  return PJ::okStatus();
}

void BridgeWebSocket::sendText(std::string text) {
  if (isOpen()) {
    state_->socket.sendText(text);
  }
}

bool BridgeWebSocket::isOpen() const {
  return state_ && state_->socket.getReadyState() == ix::ReadyState::Open;
}

void BridgeWebSocket::stop() {
  if (!state_) {
    return;
  }
  state_->sink->close();
  state_->socket.stop();
}

}  // namespace PJ::BridgeProtocol
