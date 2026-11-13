#pragma once

#include <ixwebsocket/IXWebSocket.h>

#include <cstdint>
#include <memory>
#include <pj_plugins/sdk/endpoint.hpp>
#include <string_view>

namespace PJ::FoxgloveProtocol {

/// A socket aimed at a Foxglove WebSocket server, not started yet: the caller
/// installs its message callback first, because the server sends its one-shot
/// "advertise" burst the moment the connection opens.
///
/// Offers both subprotocol names and lets the server pick: foxglove_bridge
/// releases built on the Foxglove SDK accept only "foxglove.sdk.v1", the ones
/// before it only "foxglove.websocket.v1". The messages this plugin exchanges
/// are the same under both.
[[nodiscard]] inline std::unique_ptr<ix::WebSocket> makeSocket(std::string_view address, uint16_t port) {
  auto socket = std::make_unique<ix::WebSocket>();
  socket->setUrl(PJ::sdk::composeEndpoint("ws", address, port));
  socket->addSubProtocol("foxglove.sdk.v1");
  socket->addSubProtocol("foxglove.websocket.v1");
  return socket;
}

}  // namespace PJ::FoxgloveProtocol
