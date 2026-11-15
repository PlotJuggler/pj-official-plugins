/**
 * @file websocket_event_queue_test.cpp
 * @brief The transport's backend-independent pieces: the bounded EventQueue, the URL check and
 *  the DropWarning throttle.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

using EventType = PJ::WebSocket::Event::Type;

TEST(WebSocketUrlTest, AcceptsOnlyWebSocketUrlsWithAHost) {
  EXPECT_TRUE(PJ::WebSocket::isWebSocketUrl("ws://127.0.0.1:9871"));
  EXPECT_TRUE(PJ::WebSocket::isWebSocketUrl("wss://robot.local:8443/bridge"));
  EXPECT_FALSE(PJ::WebSocket::isWebSocketUrl("ws://"));
  EXPECT_FALSE(PJ::WebSocket::isWebSocketUrl("wss:///bridge"));
  EXPECT_FALSE(PJ::WebSocket::isWebSocketUrl("http://robot:9871"));
  EXPECT_FALSE(PJ::WebSocket::isWebSocketUrl("robot:9871"));
}

TEST(EventQueueTest, PreservesArrivalOrderAcrossThreads) {
  PJ::WebSocket::EventQueue queue;
  constexpr int kCount = 1000;
  std::thread writer([&queue] {
    for (int index = 0; index < kCount; ++index) {
      queue.push({EventType::kText, std::to_string(index)});
    }
  });
  std::vector<std::string> received;
  while (received.size() < static_cast<size_t>(kCount)) {
    for (auto& event : queue.drain()) {
      received.push_back(std::move(event.payload));
    }
    std::this_thread::yield();
  }
  writer.join();
  for (int index = 0; index < kCount; ++index) {
    EXPECT_EQ(received[static_cast<size_t>(index)], std::to_string(index));
  }
}

TEST(EventQueueTest, OverflowDropsTheOldestDataFramesOnly) {
  PJ::WebSocket::EventQueue queue(2000);  // two 900-byte frames plus overhead fit, three do not
  queue.push({EventType::kBinary, std::string(900, 'a')});
  queue.push({EventType::kText, "control"});
  queue.push({EventType::kBinary, std::string(900, 'b')});
  queue.push({EventType::kBinary, std::string(900, 'c')});
  queue.push({EventType::kClosed, "bye"});
  queue.push({EventType::kText, std::string(5000, 't')});  // control frames never count against the cap
  const auto pending = queue.drain();
  ASSERT_EQ(pending.size(), 5U);
  EXPECT_EQ(pending[0].payload, "control");
  EXPECT_EQ(pending[1].payload, std::string(900, 'b'));
  EXPECT_EQ(pending[2].payload, std::string(900, 'c'));
  EXPECT_EQ(pending[3].type, EventType::kClosed);
  EXPECT_EQ(pending[4].payload.size(), 5000U);
  EXPECT_EQ(queue.dropped().frames, 1U);
  EXPECT_EQ(queue.dropped().bytes, 900U);

  // The budget frees up on drain; the count is cumulative.
  queue.push({EventType::kBinary, std::string(900, 'd')});
  EXPECT_EQ(queue.drain().size(), 1U);
  EXPECT_EQ(queue.dropped().frames, 1U);
}

TEST(EventQueueTest, OversizedFrameReplacesOlderDataButIsKept) {
  PJ::WebSocket::EventQueue queue(100);
  queue.push({EventType::kBinary, std::string(1000, 'x')});
  queue.push({EventType::kBinary, std::string(1000, 'y')});
  const auto pending = queue.drain();
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending[0].payload, std::string(1000, 'y'));
  EXPECT_EQ(queue.dropped().frames, 1U);
}

TEST(EventQueueTest, CloseDropsPendingAndLateEvents) {
  PJ::WebSocket::EventQueue queue;
  queue.push({EventType::kOpen, {}});
  queue.close();
  queue.push({EventType::kText, "late"});
  EXPECT_TRUE(queue.drain().empty());
}

TEST(DropWarningTest, ReportsTheFirstDropAtOnceThenThrottlesWithTheRunningTotal) {
  using namespace std::chrono_literals;
  PJ::WebSocket::EventQueue queue(100);
  PJ::WebSocket::DropWarning warning;
  const auto t0 = PJ::WebSocket::DropWarning::Clock::now();
  const auto at = [t0](auto offset) { return [t = t0 + offset] { return t; }; };
  EXPECT_FALSE(warning.check(queue, "test stream", at(0s)));  // nothing dropped yet

  queue.push({EventType::kBinary, std::string(1000, 'a')});
  queue.push({EventType::kBinary, std::string(1000, 'b')});  // drops 'a'
  const auto first = warning.check(queue, "test stream", at(0s));
  ASSERT_TRUE(first);
  EXPECT_EQ(
      *first,
      "PlotJuggler fell behind the test stream and dropped the oldest 1 data frames (0.0 MiB) so far; "
      "the plots have gaps where they were.");
  EXPECT_FALSE(warning.check(queue, "test stream", at(10s)));  // no new drop

  queue.push({EventType::kBinary, std::string(1000, 'c')});   // drops 'b'
  EXPECT_FALSE(warning.check(queue, "test stream", at(1s)));  // within the interval
  const auto second = warning.check(queue, "test stream", at(6s));
  ASSERT_TRUE(second);
  EXPECT_NE(second->find("oldest 2 data frames"), std::string::npos);

  warning = {};
  EXPECT_TRUE(warning.check(queue, "test stream", at(7s)));  // a fresh one reports the total again
}

}  // namespace
