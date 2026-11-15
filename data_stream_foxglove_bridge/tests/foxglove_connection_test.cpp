/**
 * @file foxglove_connection_test.cpp
 * @brief FoxgloveSource and FoxgloveDialog against the fake transport
 *  (common/websocket/testing/fake_websocket_client.cpp) and a recording runtime host: start()
 *  never waits for the connection, every event is applied in arrival order on the poll thread,
 *  a failed first connection stops the stream once, a lost one is retried, and the dialog hands
 *  its open connection to the source.
 *
 * The plugin translation unit is included so its anonymous-namespace classes are reachable.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <pj_websocket/testing/fake_websocket_client.hpp>
#include <string>
#include <test_support/recording_runtime_host.hpp>
#include <thread>
#include <vector>

#include "../foxglove_source.cpp"  // NOLINT(bugprone-suspicious-include)

namespace {

using PJ::WebSocket::testing::FakeSocket;
using PJ::WebSocket::testing::fakeSockets;
using toolbox_testing::RecordingRuntimeHost;
using EventType = PJ::WebSocket::Event::Type;

/// Test-length deadlines; the queue keeps its platform default.
FoxgloveConnectionOptions testOptions() {
  FoxgloveConnectionOptions options;
  options.connect = std::chrono::milliseconds(50);
  options.retry_first = std::chrono::milliseconds(20);
  options.retry_max = std::chrono::milliseconds(80);
  options.reconnect = std::chrono::milliseconds(30);
  return options;
}

/// For tests that poll a source whose socket is still connecting: a deadline no slow runner
/// reaches. Only the timeout tests keep the short one.
FoxgloveConnectionOptions patientOptions() {
  auto options = testOptions();
  options.connect = std::chrono::seconds(10);
  return options;
}

/// One FoxgloveSource bound to a recording host, with test-length deadlines.
struct FoxgloveFixture {
  explicit FoxgloveFixture(bool demand_capable = true, FoxgloveConnectionOptions options = testOptions())
      : host(demand_capable), source(options) {
    fakeSockets().clear();
    write_vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    write_vtable.struct_size = sizeof(write_vtable);
    registry.registerService<PJ::sdk::SourceWriteHostService>(PJ_source_write_host_t{&write_vtable, &write_vtable});
    registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(host.raw());
    EXPECT_TRUE(source.bind(PJ::sdk::ServiceRegistry(registry.view())));
  }

  [[nodiscard]] FakeSocket& lastSocket() const {
    EXPECT_FALSE(fakeSockets().empty());
    return *fakeSockets().back();
  }

  /// Loads `config`, starts the source and returns its socket.
  [[nodiscard]] FakeSocket& startWith(const nlohmann::json& config) {
    EXPECT_TRUE(source.loadConfig(config.dump()));
    EXPECT_TRUE(source.start());
    EXPECT_EQ(fakeSockets().size(), 1U);
    return lastSocket();
  }

  /// The source's embedded dialog, as the host reaches it through getDialog().
  [[nodiscard]] FoxgloveDialog& dialog() {
    return *static_cast<FoxgloveDialog*>(static_cast<PJ::DialogPluginBase*>(source.getDialog().ctx));
  }

  void setActiveTopics(const std::vector<std::string>& names) {
    std::vector<PJ_string_view_t> views;
    for (const auto& name : names) {
      views.push_back({name.data(), name.size()});
    }
    const auto* extension =
        static_cast<const PJ_topic_subscription_v1_t*>(source.pluginExtension(PJ_TOPIC_SUBSCRIPTION_EXTENSION_V1));
    ASSERT_NE(extension, nullptr);
    PJ_error_t error{};
    ASSERT_TRUE(extension->set_active_topics(&source, views.data(), views.size(), &error));
  }

  RecordingRuntimeHost host;
  PJ_source_write_host_vtable_t write_vtable{};
  PJ::ServiceRegistryBuilder registry;
  FoxgloveSource source;
};

nlohmann::json channel(uint64_t id, const std::string& topic) {
  return {
      {"id", id},
      {"topic", topic},
      {"encoding", "cdr"},
      {"schemaName", "std_msgs/msg/Float64"},
      {"schema", "float64 data"},
      {"schemaEncoding", "ros2msg"}};
}

std::string advertise(const std::vector<nlohmann::json>& channels) {
  return nlohmann::json{{"op", "advertise"}, {"channels", channels}}.dump();
}

/// A Foxglove message-data frame: opcode 0x01, subscription id, log time, payload.
std::string messageData(uint32_t subscription_id, uint64_t log_time_ns, const std::string& payload) {
  std::string frame(1, '\x01');
  char bytes[12];
  std::memcpy(bytes, &subscription_id, 4);
  std::memcpy(bytes + 4, &log_time_ns, 8);
  frame.append(bytes, sizeof(bytes));
  return frame + payload;
}

/// The "op" of every frame the client sent, in order.
std::vector<std::string> ops(const FakeSocket& socket) {
  std::vector<std::string> names;
  for (const auto& text : socket.sent) {
    names.push_back(nlohmann::json::parse(text).value("op", std::string{}));
  }
  return names;
}

// --- Connection lifecycle ---

TEST(FoxgloveConnectionTest, StartReturnsBeforeConnectionAndOffersBothSubprotocols) {
  FoxgloveFixture fixture(/*demand_capable=*/true, patientOptions());
  const auto started = std::chrono::steady_clock::now();
  FakeSocket& socket = fixture.startWith({{"address", "10.0.0.2"}, {"port", 8765}});
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));

  EXPECT_EQ(socket.url, "ws://10.0.0.2:8765");
  // foxglove_bridge releases built on the Foxglove SDK accept only the first, older ones only the second.
  EXPECT_EQ(socket.subprotocols, (std::vector<std::string>{"foxglove.sdk.v1", "foxglove.websocket.v1"}));
  EXPECT_TRUE(fixture.source.poll());  // still connecting: quiet
}

TEST(FoxgloveConnectionTest, ExplicitWssUrlIsUsedVerbatim) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"url", "wss://robot.local:8765/foxglove"}});
  EXPECT_EQ(socket.url, "wss://robot.local:8765/foxglove");
}

TEST(FoxgloveConnectionTest, RejectsAUrlThatIsNotAWebSocketUrl) {
  FoxgloveFixture fixture;
  EXPECT_FALSE(fixture.source.loadConfig(R"({"url":"http://robot:8765"})"));
}

TEST(FoxgloveConnectionTest, ConnectTimeoutFailsOnce) {
  FoxgloveFixture fixture;
  (void)fixture.startWith({{"address", "robot"}});

  std::this_thread::sleep_for(testOptions().connect + std::chrono::milliseconds(20));
  EXPECT_FALSE(fixture.source.poll());
  EXPECT_FALSE(fixture.source.poll());  // latched, not re-reported
  EXPECT_EQ(fakeSockets().size(), 1U);  // the first connection is not retried
}

TEST(FoxgloveConnectionTest, RefusedFirstConnectionStopsTheStream) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});
  socket.sink->push({EventType::kError, "connection refused"});
  EXPECT_FALSE(fixture.source.poll());
}

// --- Streaming ---

TEST(FoxgloveConnectionTest, DemandModeAdvertisesSubscribesAndPushesInArrivalOrder) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});

  // The advertise burst lands right behind the open, before the first poll.
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed"), channel(8, "/accel")}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/speed", "/accel"}));
  EXPECT_TRUE(socket.sent.empty());  // demand mode subscribes nothing on its own

  fixture.setActiveTopics({"/speed"});
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(ops(socket), (std::vector<std::string>{"subscribe"}));
  const auto subscribe = nlohmann::json::parse(socket.sent.back());
  EXPECT_EQ(subscribe["subscriptions"][0]["channelId"], 7);
  const uint32_t sub_id = subscribe["subscriptions"][0]["id"].get<uint32_t>();

  // The pushed payload is a view into the queued frame: a frame short enough for the string's
  // inline buffer (13-byte header + 2) and a heap-allocated one must both arrive intact, even
  // when the host fetches them only after the poll has returned.
  fixture.host.defer_fetch = true;
  const std::string large(4096, 'z');
  socket.deliverBinary(messageData(sub_id, 1'000'000'000, "abc"));
  socket.deliverBinary(messageData(sub_id + 99, 2, "unknown subscription"));
  socket.deliverBinary(messageData(sub_id, 3, "xy"));
  socket.deliverBinary(messageData(sub_id, 4, large));
  ASSERT_TRUE(fixture.source.poll());
  socket.deliverBinary(std::string(256, '\x7f'));  // reuses the memory a dangling payload would point at
  ASSERT_TRUE(fixture.source.poll());
  fixture.host.fetchDeferred();
  ASSERT_EQ(fixture.host.pushes.size(), 3U);
  EXPECT_EQ(fixture.host.pushes[0].timestamp_ns, 1'000'000'000);
  EXPECT_EQ(fixture.host.pushes[0].bytes, "abc");
  EXPECT_EQ(fixture.host.pushes[1].bytes, "xy");
  EXPECT_EQ(fixture.host.pushes[2].bytes, large);
}

// Data and removals share one FIFO: a message queued before its channel's unadvertise is
// ingested, one queued after it is not, even within one poll.
TEST(FoxgloveConnectionTest, UnadvertiseBetweenTwoMessagesOfOneBatchStopsOnlyTheSecond) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed")}));
  fixture.setActiveTopics({"/speed"});
  ASSERT_TRUE(fixture.source.poll());
  const uint32_t sub_id = nlohmann::json::parse(socket.sent.back())["subscriptions"][0]["id"].get<uint32_t>();

  socket.deliverBinary(messageData(sub_id, 1, "before"));
  socket.deliverText(R"({"op":"unadvertise","channelIds":[7]})");
  socket.deliverBinary(messageData(sub_id, 2, "after"));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fixture.host.pushes.size(), 1U);
  EXPECT_EQ(fixture.host.pushes[0].bytes, "before");
}

// The ids saved with the selection belong to an earlier server session: a fresh connection
// subscribes the selected topics by name, from the server's advertise, once each.
TEST(FoxgloveConnectionTest, LegacyHostSubscribesTheSavedSelectionFromTheAdvertise) {
  FoxgloveFixture fixture(/*demand_capable=*/false);
  nlohmann::json config{{"address", "robot"}};
  config["channels"] = nlohmann::json::array(
      {{{"id", 7},
        {"topic", "/speed"},
        {"encoding", "cdr"},
        {"schema_name", "std_msgs/msg/Float64"},
        {"schema", "float64 data"},
        {"schema_encoding", "ros2msg"}}});
  FakeSocket& socket = fixture.startWith(config);
  EXPECT_TRUE(socket.sent.empty());  // nothing can be sent before the socket opens

  socket.acceptConnection();
  socket.deliverText(advertise({channel(42, "/speed")}));  // the server renumbered the channel
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(ops(socket), (std::vector<std::string>{"subscribe"}));
  const auto subscribe = nlohmann::json::parse(socket.sent.back());
  ASSERT_EQ(subscribe["subscriptions"].size(), 1U);
  EXPECT_EQ(subscribe["subscriptions"][0]["channelId"], 42);
}

TEST(FoxgloveConnectionTest, MalformedControlFramesAreDroppedWithoutStoppingTheStream) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});
  socket.acceptConnection();
  // Malformed advertise entries and unadvertise ids are skipped; a frame whose handler still
  // throws (a wrong-typed status level) is dropped with a warning.
  socket.deliverText(R"({"op":"advertise","channels":[{"id":"seven","topic":5}, 3]})");
  socket.deliverText(R"({"op":"unadvertise","channelIds":["bad"]})");
  socket.deliverText(R"({"op":"status","level":"high","message":"x"})");
  socket.deliverText(advertise({channel(7, "/speed")}));
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(fixture.host.reported("Dropped malformed control frame"));
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/speed"}));
}

TEST(FoxgloveConnectionTest, UnadvertiseSkipsAWrongTypedIdAndAppliesTheRest) {
  FoxgloveFixture fixture;
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed"), channel(8, "/accel")}));
  ASSERT_TRUE(fixture.source.poll());

  socket.deliverText(R"({"op":"unadvertise","channelIds":[7,"bad"]})");
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(fixture.host.reported("Server removed 1 channel(s): /speed"));
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/accel"}));
}

TEST(FoxgloveConnectionTest, DroppedFramesAreReportedAgainAfterAReconnection) {
  auto options = testOptions();
  options.inbound_queue_bytes = 4096;  // room for three ~1 KB frames
  FoxgloveFixture fixture(/*demand_capable=*/true, options);
  const std::string frame = messageData(1, 1, std::string(1024, 'x'));
  const auto overflow = [&](FakeSocket& socket) {
    socket.acceptConnection();
    for (int i = 0; i < 8; ++i) {
      socket.deliverBinary(frame);
    }
    ASSERT_TRUE(fixture.source.poll());
  };

  FakeSocket& first = fixture.startWith({{"address", "robot"}});
  overflow(first);
  EXPECT_EQ(fixture.host.countReported("dropped the oldest"), 1);

  first.closeFromServer("bridge restarting");
  ASSERT_TRUE(fixture.source.poll());
  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fakeSockets().size(), 2U);
  // The new connection's queue counts from zero: the same number of drops is news again.
  overflow(fixture.lastSocket());
  EXPECT_EQ(fixture.host.countReported("dropped the oldest"), 2);
}

TEST(FoxgloveConnectionTest, LostConnectionIsRetriedAndStartsAFreshSession) {
  FoxgloveFixture fixture;
  FakeSocket& first = fixture.startWith({{"address", "robot"}});
  first.acceptConnection();
  first.deliverText(advertise({channel(7, "/speed")}));
  fixture.setActiveTopics({"/speed"});
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(ops(first), (std::vector<std::string>{"subscribe"}));

  first.closeFromServer("bridge restarting");
  ASSERT_TRUE(fixture.source.poll());  // a loss mid-stream does not stop the stream
  EXPECT_EQ(fixture.host.countReported("Foxglove bridge connection lost"), 1);
  EXPECT_TRUE(first.stopped);

  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fakeSockets().size(), 2U);
  FakeSocket& second = fixture.lastSocket();
  EXPECT_EQ(second.subprotocols, first.subprotocols);

  // The server re-advertises on the new connection with new channel ids; the host's desired
  // set survives and is re-applied against them.
  second.acceptConnection();
  second.deliverText(advertise({channel(42, "/speed")}));
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(fixture.host.reported("Reconnected to Foxglove bridge"));
  ASSERT_EQ(ops(second), (std::vector<std::string>{"subscribe"}));
  const auto subscribe = nlohmann::json::parse(second.sent.back());
  EXPECT_EQ(subscribe["subscriptions"][0]["channelId"], 42);
  EXPECT_EQ(subscribe["subscriptions"][0]["id"], 1);  // subscription ids restart per session
}

TEST(FoxgloveConnectionTest, ReconnectingToAServerWithoutChannelsEmptiesTheHostsCatalog) {
  FoxgloveFixture fixture;
  FakeSocket& first = fixture.startWith({{"address", "robot"}});
  first.acceptConnection();
  first.deliverText(advertise({channel(7, "/speed")}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/speed"}));

  first.closeFromServer("bridge restarting");
  ASSERT_TRUE(fixture.source.poll());
  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fakeSockets().size(), 2U);
  fixture.lastSocket().acceptConnection();
  fixture.lastSocket().deliverText(advertise({}));
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(fixture.host.advertisements.back().empty());
}

TEST(FoxgloveConnectionTest, FailedReconnectionIsRetriedAgain) {
  FoxgloveFixture fixture;
  FakeSocket& first = fixture.startWith({{"address", "robot"}});
  first.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  first.closeFromServer("");
  ASSERT_TRUE(fixture.source.poll());

  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fakeSockets().size(), 2U);
  fixture.lastSocket().sink->push({EventType::kError, "connection refused"});
  ASSERT_TRUE(fixture.source.poll());

  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(fakeSockets().size(), 3U);
  EXPECT_EQ(fixture.host.countReported("Foxglove bridge connection lost"), 1);
}

TEST(FoxgloveConnectionTest, StopUnsubscribesAndDropsLateEvents) {
  FoxgloveFixture fixture(/*demand_capable=*/true, patientOptions());
  FakeSocket& socket = fixture.startWith({{"address", "robot"}});
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed")}));
  fixture.setActiveTopics({"/speed"});
  ASSERT_TRUE(fixture.source.poll());

  fixture.source.stop();
  EXPECT_EQ(ops(socket), (std::vector<std::string>{"subscribe", "unsubscribe"}));
  EXPECT_TRUE(socket.stopped);
  // The fake keeps its sink open; the source must still ignore it once a new session starts: a
  // late close would make the new session's first connection fatal.
  ASSERT_TRUE(fixture.source.start());
  socket.closeFromServer("late close of the previous session");
  EXPECT_TRUE(fixture.source.poll());
  EXPECT_FALSE(fixture.host.reported("failed to connect"));
}

// --- Dialog ---

TEST(FoxgloveConnectionTest, DialogConnectionIsHandedToTheSource) {
  FoxgloveFixture fixture;
  FoxgloveDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditAddress", "wss://robot.local:8765");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  FakeSocket& socket = fixture.lastSocket();
  EXPECT_EQ(socket.url, "wss://robot.local:8765");
  EXPECT_EQ(socket.subprotocols, (std::vector<std::string>{"foxglove.sdk.v1", "foxglove.websocket.v1"}));

  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed"), channel(8, "/accel")}));
  EXPECT_TRUE(dialog.onTick());
  const std::string ui = dialog.widget_data();
  EXPECT_NE(ui.find("/speed"), std::string::npos);
  EXPECT_NE(ui.find("Connected to wss://robot.local:8765"), std::string::npos) << ui;

  // OK: the source adopts the open connection instead of dialing again, and lists the whole
  // catalog the dialog discovered (the server does not re-advertise).
  ASSERT_TRUE(fixture.source.start());
  EXPECT_EQ(fakeSockets().size(), 1U);
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.front(), (std::vector<std::string>{"/speed", "/accel"}));

  fixture.setActiveTopics({"/accel"});
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(ops(socket), (std::vector<std::string>{"subscribe"}));
  EXPECT_EQ(nlohmann::json::parse(socket.sent.back())["subscriptions"][0]["channelId"], 8);
  EXPECT_EQ(nlohmann::json::parse(fixture.source.saveConfig()).value("url", std::string{}), socket.url);
}

// Retry and backoff are DialogConnection's, covered by the PJ Bridge tests; what is Foxglove's
// own is that a dropped connection clears the discovered catalog.
TEST(FoxgloveConnectionTest, EventsTheDialogHasNotDrainedReachTheSource) {
  FoxgloveFixture fixture;
  FoxgloveDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditAddress", "robot");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  FakeSocket& socket = fixture.lastSocket();
  socket.acceptConnection();
  ASSERT_TRUE(dialog.onTick());                           // the dialog sees the open...
  socket.deliverText(advertise({channel(7, "/speed")}));  // ...but not the burst behind it

  ASSERT_TRUE(fixture.source.start());
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/speed"}));
}

TEST(FoxgloveConnectionTest, LegacyHostSubscribesTheDialogSelectionOnAnAdoptedConnection) {
  FoxgloveFixture fixture(/*demand_capable=*/false);
  FoxgloveDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditAddress", "robot");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  FakeSocket& socket = fixture.lastSocket();
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed"), channel(8, "/accel")}));
  ASSERT_TRUE(dialog.onTick());
  dialog.onSelectionChanged("topicsList", {"/accel"});
  dialog.onAccepted("{}");

  ASSERT_TRUE(fixture.source.start());
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(ops(socket), (std::vector<std::string>{"subscribe"}));
  EXPECT_EQ(nlohmann::json::parse(socket.sent.back())["subscriptions"][0]["channelId"], 8);
}

TEST(FoxgloveConnectionTest, AChannelRemovedWhileTheDialogIsOpenIsNotHandedToTheSource) {
  FoxgloveFixture fixture;
  FoxgloveDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditAddress", "robot");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  FakeSocket& socket = fixture.lastSocket();
  socket.acceptConnection();
  socket.deliverText(advertise({channel(7, "/speed"), channel(8, "/accel")}));
  ASSERT_TRUE(dialog.onTick());
  socket.deliverText(R"({"op":"unadvertise","channelIds":[7]})");
  ASSERT_TRUE(dialog.onTick());
  EXPECT_EQ(dialog.widget_data().find("/speed"), std::string::npos);
  dialog.onAccepted("{}");

  ASSERT_TRUE(fixture.source.start());
  fixture.setActiveTopics({"/speed", "/accel"});
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/accel"}));
  ASSERT_EQ(ops(socket), (std::vector<std::string>{"subscribe"}));
  const auto subscribe = nlohmann::json::parse(socket.sent.back());
  ASSERT_EQ(subscribe["subscriptions"].size(), 1U);
  EXPECT_EQ(subscribe["subscriptions"][0]["channelId"], 8);
}

TEST(FoxgloveConnectionTest, AnAdoptedConnectionIsRedialledWithTheFoxgloveSubprotocols) {
  FoxgloveFixture fixture;
  FoxgloveDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditAddress", "robot");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  fixture.lastSocket().acceptConnection();
  ASSERT_TRUE(dialog.onTick());
  ASSERT_TRUE(fixture.source.start());
  ASSERT_TRUE(fixture.source.poll());

  fixture.lastSocket().closeFromServer("bridge restarted");
  ASSERT_TRUE(fixture.source.poll());
  std::this_thread::sleep_for(testOptions().reconnect + std::chrono::milliseconds(10));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fakeSockets().size(), 2U);
  EXPECT_EQ(fixture.lastSocket().subprotocols, PJ::FoxgloveProtocol::subprotocols());
  EXPECT_EQ(fixture.lastSocket().url, fakeSockets().front()->url);
}

TEST(FoxgloveConnectionTest, DialogClearsTheCatalogWhenTheConnectionDrops) {
  fakeSockets().clear();
  FoxgloveDialog dialog(testOptions());
  dialog.onTextChanged("lineEditAddress", "robot");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  const auto socket = fakeSockets().back();
  socket->acceptConnection();
  socket->deliverText(advertise({channel(7, "/speed")}));
  EXPECT_TRUE(dialog.onTick());
  EXPECT_EQ(dialog.allDiscoveredChannels().size(), 1U);

  socket->closeFromServer("bridge restarting");
  EXPECT_TRUE(dialog.onTick());
  EXPECT_TRUE(dialog.allDiscoveredChannels().empty());
  EXPECT_NE(dialog.widget_data().find("Retrying in 1 s: bridge restarting"), std::string::npos);
}

TEST(FoxgloveConnectionTest, DialogKeepsAddressAndPortConfigCompatible) {
  FoxgloveDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"address":"10.0.0.2","port":9000})"));
  EXPECT_EQ(dialog.endpoint(), "ws://10.0.0.2:9000");
  ASSERT_TRUE(dialog.loadConfig(R"({})"));
  EXPECT_EQ(dialog.endpoint(), "ws://localhost:8765");
  EXPECT_FALSE(dialog.loadConfig(R"({"port":"not a number"})"));
  EXPECT_FALSE(dialog.loadConfig(R"({"port":70000})"));  // not truncated to 4464
  EXPECT_FALSE(dialog.loadConfig(R"({"port":-1})"));
  ASSERT_TRUE(dialog.loadConfig(R"({"port":65535})"));
  EXPECT_EQ(dialog.endpoint(), "ws://localhost:65535");
}

// The transport indicator follows the URL's scheme, so switching it must refresh the widgets.
TEST(FoxgloveConnectionTest, DialogRefreshesWhenTheUrlSchemeChanges) {
  FoxgloveDialog dialog;
  EXPECT_FALSE(dialog.onTextChanged("lineEditAddress", "robot"));
  EXPECT_TRUE(dialog.onTextChanged("lineEditAddress", "ws://robot:8765"));    // address -> URL
  EXPECT_TRUE(dialog.onTextChanged("lineEditAddress", "wss://robot:8765"));   // ws -> wss
  EXPECT_FALSE(dialog.onTextChanged("lineEditAddress", "wss://robot:8766"));  // same scheme
  EXPECT_TRUE(dialog.onTextChanged("lineEditAddress", "ws://robot:8766"));    // wss -> ws
}

}  // namespace
