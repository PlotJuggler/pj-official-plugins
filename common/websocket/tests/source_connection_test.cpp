/**
 * @file source_connection_test.cpp
 * @brief SourceConnection's failure and reconnection rules, on the fake backend. The streaming
 *  sources' own connection tests cover the rest through their plugins.
 */

#include <gtest/gtest.h>

#include <chrono>
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
  std::vector<std::string> reports;

  SourceConnection::Handlers handlers() {
    return {
        [this](bool reconnected) { opens.push_back(reconnected); }, [](const std::string&) {}, [](std::string&&) {},
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
  SourceConnection connection({"Test bridge", 20ms, std::nullopt}, recorder.handlers());
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

TEST_F(SourceConnectionTest, TheFirstFailureWinsOverAQueuedClose) {
  Recorder recorder;
  SourceConnection connection({"Test bridge", 1s, std::nullopt}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  fakeSockets().at(0)->acceptConnection();
  ASSERT_TRUE(connection.poll());

  connection.fail("unsupported protocol");
  fakeSockets().at(0)->closeFromServer("bye");
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "unsupported protocol");
}

TEST_F(SourceConnectionTest, AFirstConnectionTimingOutIsFatalEvenWithReconnect) {
  Recorder recorder;
  SourceConnection connection({"Test bridge", 10ms, 10ms}, recorder.handlers());
  ASSERT_TRUE(connection.start("ws://robot:1"));
  std::this_thread::sleep_for(20ms);
  auto status = connection.poll();
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error(), "failed to connect to Test bridge at ws://robot:1: timed out");
}

TEST_F(SourceConnectionTest, ALostConnectionIsRedialledWithItsSubprotocols) {
  Recorder recorder;
  SourceConnection connection({"Test bridge", 1s, 10ms}, recorder.handlers());
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
