/**
 * @file pj_bridge_websocket_native_test.cpp
 * @brief The IXWebSocket backend honours the BridgeWebSocket contract against a loopback server:
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
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "../pj_bridge_websocket.hpp"

namespace {

using PJ::BridgeProtocol::BridgeEventQueue;
using PJ::BridgeProtocol::BridgeWebSocket;
using PJ::BridgeProtocol::BridgeWebSocketEvent;
using PJ::BridgeProtocol::firstExistingCaFile;
using EventType = BridgeWebSocketEvent::Type;
using Clock = std::chrono::steady_clock;

/// Drains `events` until an event satisfying `wanted` arrives, or gives up after five seconds.
template <typename Predicate>
std::optional<BridgeWebSocketEvent> waitFor(BridgeEventQueue& events, Predicate wanted) {
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

TEST(BridgeWebSocketNativeTest, EventsArriveThroughTheSinkAndStopDropsLateOnes) {
  ix::initNetSystem();
  EchoServer server;
  ASSERT_TRUE(server.listening());

  auto events = std::make_shared<BridgeEventQueue>();
  BridgeWebSocket socket;
  const auto started = Clock::now();
  ASSERT_TRUE(socket.start(server.url(), events));
  EXPECT_LT(Clock::now() - started, std::chrono::seconds(1));

  ASSERT_TRUE(waitFor(*events, [](const BridgeWebSocketEvent& event) { return event.type == EventType::kOpen; }));
  EXPECT_TRUE(socket.isOpen());

  socket.sendText(R"({"command":"get_topics"})");
  const auto echo = waitFor(*events, [](const BridgeWebSocketEvent& event) { return event.type == EventType::kText; });
  ASSERT_TRUE(echo);
  EXPECT_EQ(echo->payload, R"({"command":"get_topics"})");

  socket.stop();
  socket.stop();
  EXPECT_FALSE(socket.isOpen());
  // Anything the transport delivers after stop() is discarded.
  events->push({EventType::kText, "late"});
  EXPECT_TRUE(events->drain().empty());
}

TEST(BridgeWebSocketNativeTest, RefusedConnectionArrivesAsAnEvent) {
  ix::initNetSystem();
  const std::string url = "ws://127.0.0.1:" + std::to_string(ix::getFreePort());

  auto events = std::make_shared<BridgeEventQueue>();
  BridgeWebSocket socket;
  const auto started = Clock::now();
  ASSERT_TRUE(socket.start(url, events));
  EXPECT_LT(Clock::now() - started, std::chrono::seconds(1));

  const auto failure = waitFor(*events, [](const BridgeWebSocketEvent& event) {
    return event.type == EventType::kError || event.type == EventType::kClosed;
  });
  ASSERT_TRUE(failure);
  EXPECT_FALSE(socket.isOpen());
}

TEST(BridgeWebSocketNativeTest, TlsTrustsTheFirstCaBundleThatExists) {
  const std::filesystem::path dir = ::testing::TempDir();
  const std::string bundle = (dir / "pj_bridge_ca_bundle.pem").string();
  std::ofstream(bundle) << "test";
  const std::string missing = (dir / "pj_bridge_no_such_bundle.pem").string();

  EXPECT_EQ(firstExistingCaFile({missing, bundle}), bundle);
  EXPECT_EQ(firstExistingCaFile({missing}), "SYSTEM");  // IXWebSocket's own default
  std::filesystem::remove(bundle);
}

}  // namespace
