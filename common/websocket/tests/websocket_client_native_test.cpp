/**
 * @file websocket_client_native_test.cpp
 * @brief The IXWebSocket backend honours the Client contract against a loopback server:
 *  start() returns at once, every outcome arrives as an owned event in the sink, and stop() is
 *  idempotent and drops whatever lands afterwards.
 */

#include <gtest/gtest.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <thread>

namespace {

using PJ::WebSocket::Client;
using PJ::WebSocket::Event;
using PJ::WebSocket::EventQueue;
using PJ::WebSocket::firstExistingCaFile;
using EventType = Event::Type;
using Clock = std::chrono::steady_clock;

/// Drains `events` until an event satisfying `wanted` arrives, or gives up after five seconds.
template <typename Predicate>
std::optional<Event> waitFor(EventQueue& events, Predicate wanted) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline) {
    for (auto& event : events.drain()) {
      if (wanted(event)) {
        return event;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

/// Echoes every frame back to its sender.
class EchoServer {
 public:
  EchoServer() : port_(ix::getFreePort()), server_(port_, "127.0.0.1") {
    server_.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& client, const ix::WebSocketMessagePtr& message) {
          if (message->type == ix::WebSocketMessageType::Message) {
            client.send(message->str, message->binary);
          }
        });
    listening_ = server_.listen().first;
    if (listening_) {
      server_.start();
    }
  }
  ~EchoServer() {
    server_.stop();
  }
  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(const EchoServer&) = delete;

  [[nodiscard]] bool listening() const {
    return listening_;
  }
  [[nodiscard]] std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/bridge";
  }

 private:
  int port_;
  ix::WebSocketServer server_;
  bool listening_ = false;
};

TEST(WebSocketClientNativeTest, EventsArriveThroughTheSinkAndStopDropsLateOnes) {
  ix::initNetSystem();
  EchoServer server;
  ASSERT_TRUE(server.listening());

  auto events = std::make_shared<EventQueue>();
  Client socket;
  const auto started = Clock::now();
  ASSERT_TRUE(socket.start(server.url(), events));
  EXPECT_LT(Clock::now() - started, std::chrono::seconds(1));

  ASSERT_TRUE(waitFor(*events, [](const Event& event) { return event.type == EventType::kOpen; }));
  EXPECT_TRUE(socket.isOpen());

  socket.sendText(R"({"command":"get_topics"})");
  const auto echo = waitFor(*events, [](const Event& event) { return event.type == EventType::kText; });
  ASSERT_TRUE(echo);
  EXPECT_EQ(echo->payload, R"({"command":"get_topics"})");

  socket.stop();
  socket.stop();
  EXPECT_FALSE(socket.isOpen());
  // Anything the transport delivers after stop() is discarded.
  events->push({EventType::kText, "late"});
  EXPECT_TRUE(events->drain().empty());
}

TEST(WebSocketClientNativeTest, RefusedConnectionArrivesAsAnEvent) {
  ix::initNetSystem();
  const std::string url = "ws://127.0.0.1:" + std::to_string(ix::getFreePort());

  auto events = std::make_shared<EventQueue>();
  Client socket;
  const auto started = Clock::now();
  ASSERT_TRUE(socket.start(url, events));
  EXPECT_LT(Clock::now() - started, std::chrono::seconds(1));

  const auto failure = waitFor(
      *events, [](const Event& event) { return event.type == EventType::kError || event.type == EventType::kClosed; });
  ASSERT_TRUE(failure);
  EXPECT_FALSE(socket.isOpen());
}

TEST(WebSocketClientNativeTest, TlsTrustsTheFirstCaBundleThatExists) {
  const std::filesystem::path dir = ::testing::TempDir();
  const std::string bundle = (dir / "pj_websocket_ca_bundle.pem").string();
  std::ofstream(bundle) << "test";
  const std::string missing = (dir / "pj_websocket_no_such_bundle.pem").string();

  EXPECT_EQ(firstExistingCaFile({missing, bundle}), bundle);
  EXPECT_EQ(firstExistingCaFile({missing}), "SYSTEM");  // IXWebSocket's own default
  std::filesystem::remove(bundle);
}

/// The Sec-WebSocket-Protocol header of the upgrade request a Client sends when offering
/// `subprotocols`, as an in-process server sees it.
std::string offeredSubprotocols(std::vector<std::string> subprotocols) {
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

  auto events = std::make_shared<EventQueue>();
  Client socket;
  EXPECT_TRUE(socket.start("ws://127.0.0.1:" + std::to_string(port), events, std::move(subprotocols)));
  auto received = header.get_future();
  std::string result;
  if (received.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
    result = received.get();
  } else {
    ADD_FAILURE() << "the server never saw the upgrade request";
  }
  socket.stop();
  server.stop();
  std::erase(result, ' ');  // the list may be joined with or without a space
  return result;
}

// A Foxglove server picks from what the client offers: foxglove_bridge releases built on the
// Foxglove SDK accept only "foxglove.sdk.v1", the ones before it only "foxglove.websocket.v1".
TEST(WebSocketClientNativeTest, OffersTheSubprotocolsInOrder) {
  EXPECT_EQ(offeredSubprotocols({"foxglove.sdk.v1", "foxglove.websocket.v1"}), "foxglove.sdk.v1,foxglove.websocket.v1");
}

TEST(WebSocketClientNativeTest, OffersNoSubprotocolByDefault) {
  EXPECT_EQ(offeredSubprotocols({}), "");
}

}  // namespace
