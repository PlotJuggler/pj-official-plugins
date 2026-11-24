/**
 * @file source_connection_test.cpp
 * @brief SourceConnection's failure and reconnection rules, on the fake backend. The streaming
 *  sources' own connection tests cover the rest through their plugins.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <pj_websocket/source_connection.hpp>
#include <pj_websocket/testing/fake_websocket_client.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using PJ::WebSocket::SourceConnection;
using PJ::WebSocket::testing::fakeSockets;

struct Recorder {
  std::vector<bool> opens;
  std::vector<std::string> events;  ///< "open" and "text", in delivery order
  std::vector<std::string> reports;

  SourceConnection::Handlers handlers() {
    return {
        [this](bool reconnected) {
          opens.push_back(reconnected);
          events.emplace_back("open");
        },
        [this](const std::string&) { events.emplace_back("text"); }, [](std::string&&) {},
        [this](PJ::DataSourceMessageLevel, const std::string& message) { reports.push_back(message); }};
  }
};

class SourceConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fakeSockets().clear();
  }
};

TEST_F(SourceConnectionTest, ALostConnectionWithoutReconnectStopsTheStreamForGood) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 20ms, .reconnect = std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  fakeSockets().at(0)->acceptConnection();
  ASSERT_TRUE(connection.poll());
  EXPECT_TRUE(connection.isOpen());

  fakeSockets().at(0)->closeFromServer("");
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "lost connection to Test bridge at ws://robot:1: closed by the server");

  std::this_thread::sleep_for(40ms);  // past every deadline: nothing is dialled again
  EXPECT_FALSE(connection.poll());
  EXPECT_EQ(fakeSockets().size(), 1u);
  EXPECT_TRUE(fakeSockets().at(0)->stopped);
}

TEST_F(SourceConnectionTest, MessagesNameTheUrlWithoutItsSecrets) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 1s, .reconnect = std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1/b?token=t&ns=a"));
  EXPECT_EQ(fakeSockets().at(0)->url, "ws://robot:1/b?token=t&ns=a");  // dialed as given
  fakeSockets().at(0)->closeFromServer("");
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "failed to connect to Test bridge at ws://robot:1/b?ns=a: closed by the server");
}

TEST_F(SourceConnectionTest, TheFirstFailureWinsAndStopsTheSocket) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 1s, .reconnect = std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  fakeSockets().at(0)->acceptConnection();
  ASSERT_TRUE(connection.poll());

  connection.fail("unsupported protocol");
  connection.fail("a later failure");
  EXPECT_TRUE(fakeSockets().at(0)->stopped);
  EXPECT_FALSE(connection.isOpen());
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "unsupported protocol");
}

TEST_F(SourceConnectionTest, AFirstConnectionClosedWithoutAReasonSaysTheServerClosedIt) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 1s, .reconnect = 10ms}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  fakeSockets().at(0)->closeFromServer("");
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "failed to connect to Test bridge at ws://robot:1: closed by the server");
}

TEST_F(SourceConnectionTest, DroppedFramesAreReportedEvenWhenACloseFollowsThem) {
  Recorder recorder;
  SourceConnection connection(
      {.name = "Test bridge", .connect = 1s, .reconnect = std::nullopt, .inbound_queue_bytes = 8}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  auto& socket = *fakeSockets().at(0);
  socket.acceptConnection();
  socket.deliverBinary(std::string(6, 'a'));
  socket.deliverBinary(std::string(6, 'b'));  // over budget: the first frame is dropped
  socket.closeFromServer("bye");
  EXPECT_FALSE(connection.poll());
  ASSERT_FALSE(recorder.reports.empty());
  EXPECT_NE(recorder.reports.back().find("dropped the oldest 1 data frames"), std::string::npos)
      << recorder.reports.back();
}

TEST_F(SourceConnectionTest, AnAdoptedConnectionOpensOnTheFirstPollAndRedialsAsTheDialogDid) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 1s, .reconnect = 10ms}, recorder.handlers());
  PJ::WebSocket::Handoff handoff{
      std::make_unique<PJ::WebSocket::Client>(), std::make_shared<PJ::WebSocket::EventQueue>(), "ws://robot:1", {"v1"}};
  ASSERT_TRUE(handoff.socket->start(handoff.url, handoff.events, handoff.subprotocols));
  fakeSockets().at(0)->acceptConnection();
  (void)handoff.events->drain();            // the dialog applied the open...
  fakeSockets().at(0)->deliverText("{}");   // ...but not the frame behind it
  fakeSockets().at(0)->acceptConnection();  // nor a duplicate kOpen, which is not news
  ASSERT_TRUE(connection.adopt(std::move(handoff)));
  EXPECT_TRUE(recorder.opens.empty());

  ASSERT_TRUE(connection.poll());
  EXPECT_EQ(recorder.opens, std::vector<bool>{false});
  EXPECT_EQ(recorder.events, (std::vector<std::string>{"open", "text"}));

  fakeSockets().at(0)->closeFromServer("restart");
  ASSERT_TRUE(connection.poll());
  std::this_thread::sleep_for(20ms);
  ASSERT_TRUE(connection.poll());
  ASSERT_EQ(fakeSockets().size(), 2u);
  EXPECT_EQ(fakeSockets().at(1)->url, "ws://robot:1");
  EXPECT_EQ(fakeSockets().at(1)->subprotocols, std::vector<std::string>{"v1"});
}

TEST_F(SourceConnectionTest, AFirstConnectionTimingOutIsFatalEvenWithReconnect) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 10ms, .reconnect = 10ms}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  std::this_thread::sleep_for(20ms);
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "failed to connect to Test bridge at ws://robot:1: timed out");
}

// What counts is when the open arrived, not when the poll applied it.
TEST_F(SourceConnectionTest, AnOpenArrivingAfterTheDeadlineIsATimeoutEvenBeforeThePoll) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 10ms, .reconnect = std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  std::this_thread::sleep_for(20ms);
  fakeSockets().at(0)->acceptConnection();  // too late, but queued before the poll
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "failed to connect to Test bridge at ws://robot:1: timed out");
  EXPECT_TRUE(recorder.opens.empty());
}

TEST_F(SourceConnectionTest, ATimelyOpenIsAcceptedEvenWhenThePollComesLate) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 10ms, .reconnect = std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  fakeSockets().at(0)->acceptConnection();
  std::this_thread::sleep_for(20ms);  // the poll thread was busy past the deadline
  ASSERT_TRUE(connection.poll());
  EXPECT_TRUE(connection.isOpen());
  EXPECT_EQ(recorder.opens, std::vector<bool>{false});
}

TEST_F(SourceConnectionTest, ALostConnectionIsRedialledWithItsSubprotocols) {
  Recorder recorder;
  SourceConnection connection({.name = "Test bridge", .connect = 1s, .reconnect = 10ms}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1", {"v1"}));
  fakeSockets().at(0)->acceptConnection();
  ASSERT_TRUE(connection.poll());

  fakeSockets().at(0)->closeFromServer("restart");
  ASSERT_TRUE(connection.poll());
  EXPECT_FALSE(connection.isOpen());
  EXPECT_EQ(recorder.reports, std::vector<std::string>{"Test bridge connection lost"});

  std::this_thread::sleep_for(20ms);
  ASSERT_TRUE(connection.poll());
  ASSERT_EQ(fakeSockets().size(), 2u);
  EXPECT_EQ(fakeSockets().at(1)->subprotocols, std::vector<std::string>{"v1"});
  fakeSockets().at(1)->acceptConnection();
  ASSERT_TRUE(connection.poll());
  EXPECT_TRUE(connection.isOpen());
  EXPECT_EQ(recorder.opens, (std::vector<bool>{false, true}));
  EXPECT_EQ(recorder.reports.back(), "Reconnected to Test bridge");
}

}  // namespace
