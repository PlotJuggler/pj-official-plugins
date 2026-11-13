/**
 * @file foxglove_socket_test.cpp
 * @brief The WebSocket handshake a Foxglove server sees from makeSocket(),
 *  captured by an in-process server on a loopback port.
 */

#include "../foxglove_socket.hpp"

#include <gtest/gtest.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <chrono>
#include <future>
#include <pj_streaming/websocket_utils.hpp>
#include <string>

namespace {

/// The Sec-WebSocket-Protocol header of the client's upgrade request.
std::string offeredSubprotocols() {
  ix::initNetSystem();
  const int port = ix::getFreePort();
  ix::WebSocketServer server(port, "127.0.0.1");

  std::promise<std::string> header;
  server.setOnClientMessageCallback(
      [&header](const std::shared_ptr<ix::ConnectionState>&, ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          const auto it = msg->openInfo.headers.find("Sec-WebSocket-Protocol");
          header.set_value(it == msg->openInfo.headers.end() ? std::string() : it->second);
        }
      });
  if (!server.listen().first) {
    ADD_FAILURE() << "cannot listen on 127.0.0.1:" << port;
    return {};
  }
  server.start();

  auto client = PJ::FoxgloveProtocol::makeSocket("127.0.0.1", static_cast<uint16_t>(port));
  client->disableAutomaticReconnection();
  client->setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});  // ix throws on a socket without one
  EXPECT_TRUE(pj::streaming::startAndWaitForOpen(*client, std::chrono::seconds(5)));

  auto received = header.get_future();
  std::string result;
  if (received.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
    result = received.get();
  } else {
    ADD_FAILURE() << "the server never saw the upgrade request";
  }
  client->stop();
  server.stop();
  std::erase(result, ' ');  // the list may be joined with or without a space
  return result;
}

// foxglove_bridge releases built on the Foxglove SDK accept only
// "foxglove.sdk.v1"; the ones before it accept only "foxglove.websocket.v1". A
// server picks from what the client offers, so offering both reaches either.
TEST(FoxgloveSocket, OffersTheSdkSubprotocolThenTheLegacyOne) {
  EXPECT_EQ(offeredSubprotocols(), "foxglove.sdk.v1,foxglove.websocket.v1");
}

}  // namespace
