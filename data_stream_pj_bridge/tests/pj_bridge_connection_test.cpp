/**
 * @file pj_bridge_connection_test.cpp
 * @brief PjBridgeSource connection lifecycle against the fake transport
 *  (common/websocket/testing/fake_websocket_client.cpp) and a recording runtime host: start() never waits for the
 *  connection, onPoll() drives connecting -> discovery -> streaming, failures latch once, and
 *  stop() isolates sessions. Also covers the dialog handoff, explicit and legacy endpoints,
 *  compressed frames, heartbeat, pause/resume, demand subscriptions and the inbound-queue bound
 *  (the queue itself is tested in common/websocket/tests).
 *
 * The plugin translation unit is included so its anonymous-namespace classes are reachable.
 */

#include <gtest/gtest.h>
#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <pj_base/sdk/settings_store_host.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <pj_websocket/testing/fake_websocket_client.hpp>
#include <random>
#include <set>
#include <string>
#include <test_support/bind_scoped_registry.hpp>
#include <test_support/recording_runtime_host.hpp>
#include <test_support/widget_table.hpp>
#include <thread>
#include <vector>

#include "../pj_bridge_source.cpp"  // NOLINT(bugprone-suspicious-include)

namespace {

using PJ::WebSocket::testing::FakeSocket;
using PJ::WebSocket::testing::fakeSockets;
using toolbox_testing::rowOf;
using toolbox_testing::shownSelection;
using toolbox_testing::table;
using EventType = PJ::WebSocket::Event::Type;

/// Test-length deadlines; the queue keeps its platform default.
BridgeConnectionOptions testOptions() {
  BridgeConnectionOptions options;
  options.connect = std::chrono::milliseconds(50);
  options.subscribe = std::chrono::milliseconds(50);
  options.retry_first = std::chrono::milliseconds(20);
  options.retry_max = std::chrono::milliseconds(80);
  return options;
}

/// For tests that poll a source whose socket is still connecting: a deadline no slow runner
/// reaches. Only the timeout tests keep the short one.
BridgeConnectionOptions patientOptions() {
  auto options = testOptions();
  options.connect = std::chrono::seconds(10);
  return options;
}

/// Incompressible bytes, so a frame's queued size is predictable.
std::string noise(size_t size, uint32_t seed) {
  std::mt19937 generator(seed);
  std::string bytes(size, '\0');
  for (auto& byte : bytes) {
    byte = static_cast<char>(generator() & 0xFFU);
  }
  return bytes;
}

using FakeRuntimeHost = toolbox_testing::RecordingRuntimeHost;

/// One PjBridgeSource bound to a recording host, with test-length deadlines.
struct BridgeFixture {
  explicit BridgeFixture(
      bool demand_capable = true, BridgeConnectionOptions options = testOptions(),
      std::optional<PJ_settings_store_t> settings = std::nullopt)
      : host(demand_capable), source(options) {
    fakeSockets().clear();
    write_vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    write_vtable.struct_size = sizeof(write_vtable);
    registry.builder.registerService<PJ::sdk::SourceWriteHostService>(
        PJ_source_write_host_t{&write_vtable, &write_vtable});
    registry.builder.registerService<PJ::sdk::DataSourceRuntimeHostService>(host.raw());
    if (settings) {
      registry.builder.registerService<PJ::sdk::SettingsStoreService>(*settings);
    }
    EXPECT_TRUE(source.bind(registry.registry()));
    registry.close();
  }

  ~BridgeFixture() {
    EXPECT_EQ(registry.lateLookups(), 0) << "the source reached the registry after bind()";
  }

  [[nodiscard]] FakeSocket& onlySocket() const {
    EXPECT_EQ(fakeSockets().size(), 1U);
    return *fakeSockets().back();
  }

  /// Configures `url`, starts the source and returns its one socket.
  [[nodiscard]] FakeSocket& startWithUrl(std::string_view url) {
    EXPECT_TRUE(source.loadConfig(nlohmann::json{{"url", url}}.dump()));
    EXPECT_TRUE(source.start());
    return onlySocket();
  }

  /// The source's embedded dialog, as the host reaches it through getDialog().
  [[nodiscard]] PjBridgeDialog& dialog() {
    return *static_cast<PjBridgeDialog*>(static_cast<PJ::DialogPluginBase*>(source.getDialog().ctx));
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

  FakeRuntimeHost host;
  PJ_source_write_host_vtable_t write_vtable{};
  /// Valid only during bind(), like a PlotJuggler 4.0 host's.
  toolbox_testing::BindScopedRegistry registry;
  PjBridgeSource source;
};

/// The "command" of every frame the client sent, in order.
std::vector<std::string> commands(const FakeSocket& socket) {
  std::vector<std::string> names;
  for (const auto& text : socket.sent) {
    names.push_back(nlohmann::json::parse(text).value("command", std::string{}));
  }
  return names;
}

nlohmann::json lastSent(const FakeSocket& socket) {
  EXPECT_FALSE(socket.sent.empty());
  return socket.sent.empty() ? nlohmann::json{} : nlohmann::json::parse(socket.sent.back());
}

std::string getTopicsResponse(const std::vector<std::string>& names) {
  nlohmann::json response;
  response["status"] = "success";
  response["protocol_version"] = 1;
  response["server"] = {{"name", "test_bridge"}, {"version", "1.0"}, {"capabilities", {"include_schemas"}}};
  response["topics"] = nlohmann::json::array();
  for (const auto& name : names) {
    response["topics"].push_back(
        {{"name", name}, {"type", "std_msgs/msg/Float64"}, {"encoding", "cdr"}, {"definition", "float64 data"}});
  }
  return response.dump();
}

std::string subscribeResponse(const std::vector<std::string>& names, const std::string& id) {
  nlohmann::json response;
  response["status"] = "success";
  response["id"] = id;
  response["protocol_version"] = 1;
  response["schemas"] = nlohmann::json::object();
  for (const auto& name : names) {
    response["schemas"][name] = {{"encoding", "cdr"}, {"definition", "float64 data"}};
  }
  return response.dump();
}

template <typename Value>
void appendLittleEndian(std::string& out, Value value) {
  char bytes[sizeof(Value)];
  std::memcpy(bytes, &value, sizeof(Value));
  out.append(bytes, sizeof(Value));
}

/// A PJRB data frame carrying one message, ZSTD-compressed as the server sends it.
std::string compressedFrame(const std::string& topic, int64_t timestamp_ns, const std::string& payload) {
  std::string body;
  appendLittleEndian(body, static_cast<uint16_t>(topic.size()));
  body += topic;
  appendLittleEndian(body, static_cast<uint64_t>(timestamp_ns));
  appendLittleEndian(body, static_cast<uint32_t>(payload.size()));
  body += payload;

  std::string compressed(ZSTD_compressBound(body.size()), '\0');
  compressed.resize(ZSTD_compress(compressed.data(), compressed.size(), body.data(), body.size(), 1));

  std::string frame;
  appendLittleEndian(frame, kMagic);
  appendLittleEndian(frame, uint32_t{1});
  appendLittleEndian(frame, static_cast<uint32_t>(body.size()));
  appendLittleEndian(frame, uint32_t{0});
  return frame + compressed;
}

// --- The four lifecycle contracts PlotJuggler's streaming manager relies on ---

TEST(PjBridgeConnectionTest, StartReturnsBeforeConnection) {
  BridgeFixture fixture(/*demand_capable=*/true, patientOptions());
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://127.0.0.1:1/bridge"})"));

  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(fixture.source.start());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));

  FakeSocket& socket = fixture.onlySocket();
  EXPECT_EQ(socket.url, "ws://127.0.0.1:1/bridge");
  EXPECT_FALSE(socket.open);
  // Still connecting: polling is quiet and nothing is sent before the socket opens.
  EXPECT_TRUE(fixture.source.poll());
  EXPECT_TRUE(socket.sent.empty());
}

TEST(PjBridgeConnectionTest, PollCompletesDiscovery) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");

  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(commands(socket), (std::vector<std::string>{"subscribe_topic_updates", "get_topics"}));

  socket.deliverText(getTopicsResponse({"/imu", "/odom"}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/imu", "/odom"}));
  EXPECT_TRUE(fixture.host.reported("Connected to test_bridge"));
}

TEST(PjBridgeConnectionTest, ConnectTimeoutFailsOnce) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871/bridge");

  std::this_thread::sleep_for(testOptions().connect + std::chrono::milliseconds(20));
  const auto timed_out = fixture.source.poll();
  ASSERT_FALSE(timed_out);
  EXPECT_NE(
      timed_out.error().find("failed to connect to PJ bridge at ws://robot:9871/bridge: timed out"), std::string::npos)
      << timed_out.error();

  // A close that lands after the timeout does not replace the first failure.
  socket.closeFromServer("late close");
  const auto after_close = fixture.source.poll();
  ASSERT_FALSE(after_close);
  EXPECT_EQ(after_close.error(), timed_out.error());
}

TEST(PjBridgeConnectionTest, LostConnectionFailsOnce) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");
  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());

  socket.closeFromServer("server shutting down");
  socket.sink->push({EventType::kError, "connection reset"});
  const auto closed = fixture.source.poll();
  ASSERT_FALSE(closed);
  EXPECT_NE(closed.error().find("lost connection to PJ bridge at ws://robot:9871"), std::string::npos)
      << closed.error();
  EXPECT_NE(closed.error().find("server shutting down"), std::string::npos) << closed.error();
  // Long past the connect deadline: an open connection never times out.
  std::this_thread::sleep_for(testOptions().connect + std::chrono::milliseconds(20));
  const auto again = fixture.source.poll();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), closed.error());
}

TEST(PjBridgeConnectionTest, StopDropsLateCallbacks) {
  BridgeFixture fixture;
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://robot:9871"})"));
  ASSERT_TRUE(fixture.source.start());
  const auto first = fakeSockets().back();
  first->acceptConnection();
  ASSERT_TRUE(fixture.source.poll());

  fixture.source.stop();
  fixture.source.stop();  // idempotent
  EXPECT_TRUE(first->stopped);

  // Callbacks that land after stop(). The fake keeps delivering on purpose; the real backends
  // also close the sink (see pj_bridge_websocket_native_test).
  first->deliverText(getTopicsResponse({"/late"}));
  first->deliverBinary(compressedFrame("/late", 1, "stale"));

  ASSERT_TRUE(fixture.source.start());
  ASSERT_EQ(fakeSockets().size(), 2U);
  const auto second = fakeSockets().back();
  second->acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_TRUE(fixture.source.poll());

  for (const auto& advertised : fixture.host.advertisements) {
    EXPECT_EQ(std::find(advertised.begin(), advertised.end(), "/late"), advertised.end());
  }
  EXPECT_TRUE(fixture.host.pushes.empty());
  EXPECT_EQ(commands(*second), (std::vector<std::string>{"subscribe_topic_updates", "get_topics"}));
}

// --- Endpoints ---

TEST(PjBridgeConnectionTest, ExplicitWssUrlWithPathIsUsedVerbatim) {
  BridgeFixture fixture;
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"wss://robot.local:8443/bridge","address":"ignored","port":1})"));
  ASSERT_TRUE(fixture.source.start());
  EXPECT_EQ(fixture.onlySocket().url, "wss://robot.local:8443/bridge");

  const auto saved = nlohmann::json::parse(fixture.source.saveConfig());
  EXPECT_EQ(saved.value("url", std::string{}), "wss://robot.local:8443/bridge");
}

TEST(PjBridgeConnectionTest, RejectsAUrlThatIsNotAWebSocketUrl) {
  BridgeFixture fixture;
  for (const char* config :
       {R"({"url":"http://robot:9871"})", R"({"url":"ws://"})", R"({"url":9871})", R"({"url":"ws://u:p@robot"})",
        R"({"url":"ws://robot#f"})"}) {
    const auto status = fixture.source.loadConfig(config);
    ASSERT_FALSE(status) << config;
    EXPECT_NE(status.error().find("url"), std::string::npos) << status.error();
  }
}

TEST(PjBridgeConnectionTest, LegacyConfigSubscribesTheSelectionOnceOpen) {
  BridgeFixture fixture(/*demand_capable=*/false);
  ASSERT_TRUE(fixture.source.loadConfig(
      R"({"address":"10.0.0.2","port":9000,"topics":[{"name":"/imu","schema_name":"sensor_msgs/msg/Imu"}]})"));
  ASSERT_TRUE(fixture.source.start());
  FakeSocket& socket = fixture.onlySocket();
  EXPECT_EQ(socket.url, "ws://10.0.0.2:9000");
  // A legacy address/port config is saved back as one url.
  const auto saved = nlohmann::json::parse(fixture.source.saveConfig());
  EXPECT_EQ(saved.value("url", std::string{}), "ws://10.0.0.2:9000");
  EXPECT_FALSE(saved.contains("address"));
  EXPECT_FALSE(saved.contains("port"));

  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(commands(socket), (std::vector<std::string>{"subscribe"}));
  const auto subscribe = lastSent(socket);
  EXPECT_EQ(subscribe.at("topics"), nlohmann::json::array({"/imu"}));

  socket.deliverText(subscribeResponse({"/imu"}, subscribe.value("id", std::string{})));
  socket.deliverBinary(compressedFrame("/imu", 42, "cdr-bytes"));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fixture.host.bindings.size(), 1U);
  EXPECT_EQ(fixture.host.bindings[0].topic, "/imu");
  EXPECT_EQ(fixture.host.bindings[0].type_name, "sensor_msgs/msg/Imu");
  ASSERT_EQ(fixture.host.pushes.size(), 1U);
  EXPECT_EQ(fixture.host.pushes[0].timestamp_ns, 42);
  EXPECT_EQ(fixture.host.pushes[0].bytes, "cdr-bytes");
}

TEST(PjBridgeConnectionTest, LegacySubscribeDeadlineWarnsWithoutStopping) {
  BridgeFixture fixture(/*demand_capable=*/false);
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://robot:9871","topics":[{"name":"/imu"}]})"));
  ASSERT_TRUE(fixture.source.start());
  fixture.onlySocket().acceptConnection();
  ASSERT_TRUE(fixture.source.poll());

  std::this_thread::sleep_for(testOptions().subscribe + std::chrono::milliseconds(20));
  EXPECT_TRUE(fixture.source.poll());
  EXPECT_TRUE(fixture.host.reported("Subscribe response not received"));
}

// --- Streaming ---

TEST(PjBridgeConnectionTest, DemandSubscriptionsWaitForTheConnectionThenFollowTheHost) {
  BridgeFixture fixture(/*demand_capable=*/true, patientOptions());
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");

  // The host asks before the socket is open: the request waits instead of being lost.
  fixture.setActiveTopics({"/imu"});
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(socket.sent.empty());

  socket.acceptConnection();
  socket.deliverText(getTopicsResponse({"/imu", "/odom"}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(commands(socket), (std::vector<std::string>{"subscribe_topic_updates", "get_topics", "subscribe"}));
  const auto subscribe = lastSent(socket);
  EXPECT_EQ(subscribe.at("topics"), nlohmann::json::array({"/imu"}));

  socket.deliverText(subscribeResponse({"/imu"}, subscribe.value("id", std::string{})));
  socket.deliverBinary(compressedFrame("/imu", 7, "imu-sample"));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_EQ(fixture.host.pushes.size(), 1U);
  EXPECT_EQ(fixture.host.pushes[0].bytes, "imu-sample");

  fixture.setActiveTopics({});
  ASSERT_TRUE(fixture.source.poll());
  const auto unsubscribe = lastSent(socket);
  EXPECT_EQ(unsubscribe.value("command", std::string{}), "unsubscribe");
  EXPECT_EQ(unsubscribe.at("topics"), nlohmann::json::array({"/imu"}));
}

TEST(PjBridgeConnectionTest, HeartbeatEveryThirtyPollsOnceOpen) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");
  socket.acceptConnection();
  for (int poll_index = 0; poll_index < 30; ++poll_index) {
    ASSERT_TRUE(fixture.source.poll());
  }
  const auto sent = commands(socket);
  EXPECT_EQ(std::count(sent.begin(), sent.end(), "heartbeat"), 1);
}

TEST(PjBridgeConnectionTest, PauseAndResumeReachTheServerOnTheNextPollOnceOpen) {
  BridgeFixture fixture(/*demand_capable=*/true, patientOptions());
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");
  ASSERT_TRUE(fixture.source.pause());
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_TRUE(socket.sent.empty());
  ASSERT_TRUE(fixture.source.resume());

  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_TRUE(fixture.source.pause());
  EXPECT_NE(lastSent(socket).value("command", std::string{}), "pause");  // the host thread sends nothing
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(lastSent(socket).value("command", std::string{}), "pause");
  ASSERT_TRUE(fixture.source.resume());
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(lastSent(socket).value("command", std::string{}), "resume");
  const auto sent = commands(socket);
  EXPECT_EQ(std::count(sent.begin(), sent.end(), "pause"), 1);  // the pause/resume while connecting cancel out
}

TEST(PjBridgeConnectionTest, PauseBeforeTheConnectionOpensIsAppliedOnOpen) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9090");
  ASSERT_TRUE(fixture.source.pause());

  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(commands(socket), (std::vector<std::string>{"subscribe_topic_updates", "get_topics", "pause"}));
}

TEST(PjBridgeConnectionTest, InboundOverflowDropsTheOldestDataAndKeepsTheSession) {
  auto options = testOptions();
  options.inbound_queue_bytes = 4096;  // room for three ~1 KB frames
  BridgeFixture fixture(/*demand_capable=*/true, options);
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://robot:9871"})"));
  ASSERT_TRUE(fixture.source.start());
  FakeSocket& socket = fixture.onlySocket();
  socket.acceptConnection();
  socket.deliverText(getTopicsResponse({"/imu"}));
  fixture.setActiveTopics({"/imu"});
  ASSERT_TRUE(fixture.source.poll());
  socket.deliverText(subscribeResponse({"/imu"}, lastSent(socket).value("id", std::string{})));
  ASSERT_TRUE(fixture.source.poll());

  // The poll thread stalls while ten frames and one control frame arrive.
  constexpr int kFrames = 10;
  for (int index = 0; index < kFrames; ++index) {
    socket.deliverBinary(compressedFrame("/imu", index, noise(1000, static_cast<uint32_t>(index))));
    if (index == 5) {
      socket.deliverText(getTopicsResponse({"/imu", "/odom"}));
    }
  }
  ASSERT_TRUE(fixture.source.poll());  // the session survives

  const auto& pushes = fixture.host.pushes;
  ASSERT_FALSE(pushes.empty());
  ASSERT_LT(pushes.size(), static_cast<size_t>(kFrames));
  EXPECT_EQ(pushes.back().timestamp_ns, kFrames - 1);  // the newest frame is kept
  EXPECT_GT(pushes.front().timestamp_ns, 0);           // the oldest ones are dropped
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/imu", "/odom"}));  // control kept
  const size_t dropped = static_cast<size_t>(kFrames) - pushes.size();
  EXPECT_EQ(fixture.host.countReported("dropped"), 1);
  EXPECT_TRUE(fixture.host.reported("dropped the oldest " + std::to_string(dropped) + " data frames"));

  // Another stall inside the warning interval adds to the count without a second warning.
  for (int index = 0; index < kFrames; ++index) {
    socket.deliverBinary(compressedFrame("/imu", 100 + index, noise(1000, static_cast<uint32_t>(100 + index))));
  }
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(fixture.host.countReported("dropped"), 1);
  EXPECT_EQ(fixture.host.pushes.back().timestamp_ns, 100 + kFrames - 1);
}

// --- Dialog ---

TEST(PjBridgeConnectionTest, DialogConnectionIsHandedToTheSource) {
  BridgeFixture fixture;
  PjBridgeDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditUrl", "wss://robot.local:8443/bridge");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  FakeSocket& socket = fixture.onlySocket();
  EXPECT_EQ(socket.url, "wss://robot.local:8443/bridge");

  socket.acceptConnection();
  EXPECT_TRUE(dialog.onTick());
  EXPECT_EQ(commands(socket), (std::vector<std::string>{"get_topics"}));
  socket.deliverText(getTopicsResponse({"/imu"}));
  dialog.onTick();
  EXPECT_NE(dialog.widget_data().find("/imu"), std::string::npos);

  // OK: the source adopts the open connection instead of dialing again.
  ASSERT_TRUE(fixture.source.start());
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(fakeSockets().size(), 1U);
  EXPECT_EQ(commands(socket), (std::vector<std::string>{"get_topics", "subscribe_topic_updates", "get_topics"}));
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.front(), (std::vector<std::string>{"/imu"}));
  EXPECT_EQ(nlohmann::json::parse(fixture.source.saveConfig()).value("url", std::string{}), socket.url);
}

TEST(PjBridgeConnectionTest, DialogRetriesARefusedConnectionUntilTheServerAppears) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://robot:9871");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  ASSERT_EQ(fakeSockets().size(), 1U);
  EXPECT_NE(dialog.widget_data().find("Connecting to ws://robot:9871"), std::string::npos) << dialog.widget_data();

  fakeSockets().back()->sink->push({EventType::kError, "connection refused"});
  EXPECT_TRUE(dialog.onTick());
  const std::string retrying = dialog.widget_data();
  EXPECT_NE(retrying.find("Retrying in 1 s: connection refused"), std::string::npos) << retrying;
  EXPECT_TRUE(fakeSockets().back()->stopped);

  std::this_thread::sleep_for(testOptions().retry_first + std::chrono::milliseconds(10));
  EXPECT_TRUE(dialog.onTick());
  ASSERT_EQ(fakeSockets().size(), 2U);  // the server is up now
  const auto second = fakeSockets().back();
  second->acceptConnection();
  EXPECT_TRUE(dialog.onTick());
  EXPECT_NE(dialog.widget_data().find("Connected"), std::string::npos);
  EXPECT_EQ(commands(*second), (std::vector<std::string>{"get_topics"}));

  // A bridge restart while the dialog is open is retried the same way.
  second->closeFromServer("bridge restarting");
  EXPECT_TRUE(dialog.onTick());
  EXPECT_NE(dialog.widget_data().find("Retrying in 1 s: bridge restarting"), std::string::npos);
}

TEST(PjBridgeConnectionTest, ConnectIsAnIconButtonInertWhileAnAttemptIsInFlight) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  const auto button = [&dialog] { return nlohmann::json::parse(dialog.widget_data())["buttonConnect"]; };
  EXPECT_EQ(button().value("button_icon_name", std::string{}), "plug_connect");
  EXPECT_FALSE(button().contains("button_text"));
  EXPECT_FALSE(button().contains("checked"));
  EXPECT_TRUE(button().value("enabled", false));

  dialog.onTextChanged("lineEditUrl", "ws://robot:9871");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  EXPECT_FALSE(button().value("enabled", true));  // the attempt is in flight

  fakeSockets().back()->sink->push({EventType::kError, "connection refused"});
  dialog.onTick();
  EXPECT_TRUE(button().value("enabled", false));  // the retry wait: pressing it cancels
  EXPECT_FALSE(button().contains("button_text"));
}

TEST(PjBridgeConnectionTest, DialogAttemptThatNeverAnswersTimesOutUntilCancelled) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://10.255.255.1:9871");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));

  std::this_thread::sleep_for(testOptions().connect + std::chrono::milliseconds(10));
  EXPECT_TRUE(dialog.onTick());
  EXPECT_NE(dialog.widget_data().find("timed out"), std::string::npos) << dialog.widget_data();
  EXPECT_TRUE(fakeSockets().back()->stopped);

  // Pressing the button again cancels: no further attempts.
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  std::this_thread::sleep_for(testOptions().retry_max + std::chrono::milliseconds(10));
  dialog.onTick();
  EXPECT_EQ(fakeSockets().size(), 1U);
  EXPECT_EQ(dialog.widget_data().find("Retrying"), std::string::npos);
}

TEST(PjBridgeConnectionTest, DialogTreatsAnOpenArrivingAfterTheDeadlineAsATimeout) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://robot:9090");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  std::this_thread::sleep_for(testOptions().connect + std::chrono::milliseconds(10));
  fakeSockets().back()->acceptConnection();  // too late, but queued before the tick
  EXPECT_TRUE(dialog.onTick());
  EXPECT_NE(dialog.widget_data().find("Retrying in 1 s: timed out"), std::string::npos) << dialog.widget_data();
}

TEST(PjBridgeConnectionTest, DialogSeesACloseThatFollowsMalformedText) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://robot:9090");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  const auto socket = fakeSockets().back();
  socket->acceptConnection();
  EXPECT_TRUE(dialog.onTick());

  socket->deliverText(R"({"status":0})");  // wrong-typed field
  socket->closeFromServer("bridge restarting");
  EXPECT_TRUE(dialog.onTick());
  EXPECT_NE(dialog.widget_data().find("Retrying in 1 s: bridge restarting"), std::string::npos) << dialog.widget_data();
}

TEST(PjBridgeConnectionTest, DialogKeepsTheValidTopicsWhenOneEntryIsMalformed) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://robot:9090");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  const auto socket = fakeSockets().back();
  socket->acceptConnection();
  EXPECT_TRUE(dialog.onTick());

  auto response = nlohmann::json::parse(getTopicsResponse({"/imu"}));
  // A wrong-typed field, ahead of the valid entry so a throw would lose it.
  response["topics"].insert(response["topics"].begin(), {{"name", "/bad"}, {"type", 5}});
  socket->deliverText(response.dump());
  dialog.onTick();
  EXPECT_NE(dialog.widget_data().find("/imu"), std::string::npos) << dialog.widget_data();
}

TEST(PjBridgeConnectionTest, DialogUrlIgnoresAddressAndPortOfAnyType) {
  PjBridgeDialog dialog(testOptions());
  ASSERT_TRUE(dialog.loadConfig(R"({"url":"wss://robot.local/bridge","address":null,"port":"unused"})"));
  EXPECT_EQ(dialog.endpoint().str(), "wss://robot.local/bridge");
  EXPECT_FALSE(dialog.loadConfig(R"({"address":"10.0.0.2","port":"9090"})"));  // refused, not thrown
}

TEST(PjBridgeConnectionTest, DialogValidatesALegacyAddressAndPortAsAUrl) {
  PjBridgeDialog dialog(testOptions());
  EXPECT_FALSE(dialog.loadConfig(R"({"address":"user@robot","port":9090})"));
  EXPECT_FALSE(dialog.loadConfig(R"({"address":"robot#x"})"));
  EXPECT_FALSE(dialog.loadConfig(R"({"address":"robot","port":0})"));
  EXPECT_FALSE(dialog.loadConfig(R"({"address":"robot","port":70000})"));
  ASSERT_TRUE(dialog.loadConfig(R"({"address":"::1","port":9000})"));
  EXPECT_EQ(dialog.endpoint().str(), "ws://[::1]:9000");
}

TEST(PjBridgeConnectionTest, DialogDefaultsToTheBridgeServersPort) {
  PjBridgeDialog dialog(testOptions());
  EXPECT_EQ(dialog.endpoint().str(), "ws://127.0.0.1:9090");
  ASSERT_TRUE(dialog.loadConfig(R"({"address":"10.0.0.2"})"));
  EXPECT_EQ(dialog.endpoint().str(), "ws://10.0.0.2:9090");
}

// --- The URL line ---

TEST(PjBridgeConnectionTest, DialogCompletesABareHostWithWsAndTheDefaultPort) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", " robot.local ");
  EXPECT_EQ(dialog.endpoint().str(), "ws://robot.local:9090");
  dialog.onTextChanged("lineEditUrl", "10.0.0.2:9871/bridge");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  ASSERT_EQ(fakeSockets().size(), 1U);
  EXPECT_EQ(fakeSockets().back()->url, "ws://10.0.0.2:9871/bridge");
}

TEST(PjBridgeConnectionTest, DialogRefusesUserinfoAndDialsAQueryTokenAsTyped) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "wss://user:pw@robot:8443/b");
  const auto data = nlohmann::json::parse(dialog.widget_data());
  EXPECT_FALSE(data["buttonConnect"].value("enabled", true));
  EXPECT_NE(data["labelConnectionStatus"].dump().find("Credentials in the URL are not supported"), std::string::npos);
  dialog.onClicked("buttonConnect");
  EXPECT_TRUE(fakeSockets().empty());

  dialog.onTextChanged("lineEditUrl", "wss://robot:8443/b?token=t");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  EXPECT_EQ(fakeSockets().back()->url, "wss://robot:8443/b?token=t");
}

TEST(PjBridgeConnectionTest, DialogStatusLineHidesTheUrlsSecrets) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "ws://robot:9090/b?token=t&ns=a");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  auto status = nlohmann::json::parse(dialog.widget_data())["labelConnectionStatus"].dump();
  EXPECT_NE(status.find("Connecting to ws://robot:9090/b?ns=a..."), std::string::npos) << status;
  fakeSockets().back()->acceptConnection();
  dialog.onTick();
  status = nlohmann::json::parse(dialog.widget_data())["labelConnectionStatus"].dump();
  EXPECT_NE(status.find("Connected to ws://robot:9090/b?ns=a"), std::string::npos) << status;
  EXPECT_EQ(status.find("token"), std::string::npos) << status;
}

TEST(PjBridgeConnectionTest, DialogInvalidUrlDisablesConnectAndSaysWhy) {
  fakeSockets().clear();
  PjBridgeDialog dialog(testOptions());
  dialog.onTextChanged("lineEditUrl", "http://robot:9090");
  const auto data = nlohmann::json::parse(dialog.widget_data());
  EXPECT_FALSE(data["buttonConnect"].value("enabled", true));
  EXPECT_NE(data["labelConnectionStatus"].dump().find("ws:// or wss://"), std::string::npos) << data.dump();
  dialog.onClicked("buttonConnect");
  EXPECT_TRUE(fakeSockets().empty());

  dialog.onTextChanged("lineEditUrl", "robot");
  EXPECT_TRUE(nlohmann::json::parse(dialog.widget_data())["buttonConnect"].value("enabled", false));
}

// --- The remembered URL (pj.settings.v1) ---

TEST(PjBridgeConnectionTest, DialogStartsFromTheRememberedUrlUnlessTheConfigNamesOne) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost store(backend);
  backend.setString(PJ::WebSocket::kLastUrlSettingsKey, "ws://remembered:9000");

  BridgeFixture fixture(true, testOptions(), store.view());
  EXPECT_EQ(fixture.dialog().endpoint().str(), "ws://remembered:9000");
  ASSERT_TRUE(fixture.source.loadConfig(R"({"topics":[]})"));  // no endpoint: the remembered URL stays
  EXPECT_EQ(fixture.dialog().endpoint().str(), "ws://remembered:9000");
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://configured:1"})"));
  EXPECT_EQ(fixture.dialog().endpoint().str(), "ws://configured:1");

  // A config read before the store is bound still wins.
  PjBridgeSource other(testOptions());
  ASSERT_TRUE(other.loadConfig(R"({"address":"legacy","port":2})"));
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SettingsStoreService>(store.view());
  (void)other.bind(PJ::sdk::ServiceRegistry(registry.view()));
  EXPECT_EQ(
      static_cast<PjBridgeDialog*>(static_cast<PJ::DialogPluginBase*>(other.getDialog().ctx))->endpoint().str(),
      "ws://legacy:2");
}

TEST(PjBridgeConnectionTest, DialogRemembersTheCleanedUrlOnceConnected) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost store(backend);
  BridgeFixture fixture(true, testOptions(), store.view());
  PjBridgeDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditUrl", " robot/b?token=t&ns=a ");
  EXPECT_FALSE(backend.getString(PJ::WebSocket::kLastUrlSettingsKey));  // not per keystroke
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  dialog.onTick();
  EXPECT_FALSE(backend.getString(PJ::WebSocket::kLastUrlSettingsKey));  // not before it connected

  fixture.onlySocket().acceptConnection();
  dialog.onTick();
  EXPECT_EQ(backend.getString(PJ::WebSocket::kLastUrlSettingsKey), "ws://robot:9090/b?ns=a");
  // The config keeps the URL normalized, its query token included.
  EXPECT_EQ(nlohmann::json::parse(dialog.saveConfig()).value("url", std::string{}), "ws://robot:9090/b?token=t&ns=a");
}

TEST(PjBridgeConnectionTest, DialogRemembersTheUrlItDialedNotTheEditedLine) {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost store(backend);
  BridgeFixture fixture(true, testOptions(), store.view());
  PjBridgeDialog& dialog = fixture.dialog();
  dialog.onTextChanged("lineEditUrl", "ws://dialed:9090");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  dialog.onTextChanged("lineEditUrl", "ws://edited:9090");
  fixture.onlySocket().acceptConnection();
  dialog.onTick();
  EXPECT_EQ(backend.getString(PJ::WebSocket::kLastUrlSettingsKey), "ws://dialed:9090");
}

TEST(PjBridgeConnectionTest, DialogWithoutASettingsStoreConnectsWithoutRemembering) {
  BridgeFixture fixture;
  EXPECT_EQ(fixture.dialog().endpoint().str(), "ws://127.0.0.1:9090");
  fixture.dialog().onTextChanged("lineEditUrl", "robot");
  ASSERT_TRUE(fixture.dialog().onClicked("buttonConnect"));
  fixture.onlySocket().acceptConnection();
  EXPECT_TRUE(fixture.dialog().onTick());
}

// --- Topic selection: Select All / Deselect All and the always-subscribed transforms ---

std::string typedTopicsResponse(const std::vector<std::pair<std::string, std::string>>& topics) {
  auto response = nlohmann::json::parse(getTopicsResponse({}));
  for (const auto& [name, type] : topics) {
    response["topics"].push_back({{"name", name}, {"type", type}, {"encoding", "ros2msg"}, {"definition", ""}});
  }
  return response.dump();
}

constexpr const char* kTf = "tf2_msgs/msg/TFMessage";
constexpr const char* kFloat = "std_msgs/msg/Float64";

/// Connects `dialog` and delivers one get_topics answer.
void connectDialog(PjBridgeDialog& dialog, const std::string& topics_response) {
  fakeSockets().clear();
  dialog.onTextChanged("lineEditUrl", "ws://robot:9090");
  ASSERT_TRUE(dialog.onClicked("buttonConnect"));
  fakeSockets().back()->acceptConnection();
  dialog.onTick();
  fakeSockets().back()->deliverText(topics_response);
  dialog.onTick();
}

std::set<std::string> savedTopics(const PjBridgeDialog& dialog) {
  std::set<std::string> names;
  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  for (const auto& t : saved["topics"]) {
    names.insert(t.value("name", std::string{}));
  }
  return names;
}

TEST(PjBridgeConnectionTest, ColumnsAreTopicAndType) {
  PjBridgeDialog dialog(testOptions());
  EXPECT_EQ(table(dialog.widget_data(), "topicsList")["headers"], (nlohmann::json{"Topic", "Type"}));
}

TEST(PjBridgeConnectionTest, SelectAllAndDeselectAllFollowTheFilter) {
  PjBridgeDialog dialog(testOptions());
  connectDialog(dialog, typedTopicsResponse({{"/imu", kFloat}, {"/odom", kFloat}, {"/odom_raw", kFloat}}));
  const auto data = nlohmann::json::parse(dialog.widget_data());
  EXPECT_EQ(data["btnSelectAll"].value("shortcut", std::string{}), "Ctrl+A");
  EXPECT_EQ(data["btnDeselectAll"].value("shortcut", std::string{}), "Ctrl+Shift+A");

  dialog.onTextChanged("lineEditFilter", "odom");
  ASSERT_TRUE(dialog.onClicked("btnSelectAll"));  // the rows the filter shows
  dialog.onTextChanged("lineEditFilter", "");
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/odom", "/odom_raw"}));
  ASSERT_TRUE(dialog.onClicked("btnSelectAll"));
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/imu", "/odom", "/odom_raw"}));

  // Deselect All drops every selection, also one the filter would hide.
  dialog.onTextChanged("lineEditFilter", "imu");
  ASSERT_TRUE(dialog.onClicked("btnDeselectAll"));
  dialog.onTextChanged("lineEditFilter", "");
  EXPECT_TRUE(shownSelection(table(dialog.widget_data(), "topicsList")).empty());
  EXPECT_TRUE(savedTopics(dialog).empty());
}

TEST(PjBridgeConnectionTest, OkNeedsOnlyTheConnectionNotASelection) {
  PjBridgeDialog dialog(testOptions());
  connectDialog(dialog, typedTopicsResponse({{"/tf", kTf}}));
  ASSERT_TRUE(dialog.onClicked("btnDeselectAll"));
  EXPECT_TRUE(nlohmann::json::parse(dialog.widget_data())["buttonBox"].value("ok_enabled", false));
}

TEST(PjBridgeConnectionTest, TransformRowsAreLockedSelectedAndTooltipped) {
  PjBridgeDialog dialog(testOptions());
  connectDialog(dialog, typedTopicsResponse({{"/tf", kTf}, {"/tf_static", kTf}, {"/imu", kFloat}}));
  const auto tbl = table(dialog.widget_data(), "topicsList");
  const int tf = rowOf(tbl, "/tf");
  const int tf_static = rowOf(tbl, "/tf_static");
  const int imu = rowOf(tbl, "/imu");
  ASSERT_GE(tf, 0);
  ASSERT_GE(tf_static, 0);
  ASSERT_GE(imu, 0);
  const auto disabled = tbl.value("disabled_rows", std::set<int>{});
  EXPECT_EQ(disabled, (std::set<int>{tf, tf_static}));
  const auto& tooltips = tbl["cell_tooltips"];
  EXPECT_EQ(tooltips.value(std::to_string(tf) + ",0", std::string{}), PJ::AlwaysIncluded::kTooltip);
  EXPECT_EQ(tooltips.value(std::to_string(tf_static) + ",0", std::string{}), PJ::AlwaysIncluded::kTooltip);
  EXPECT_FALSE(tooltips.contains(std::to_string(imu) + ",0"));
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf", "/tf_static"}));
  // Saved too, so a host that subscribes exactly the saved selection subscribes them.
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/tf", "/tf_static"}));
}

TEST(PjBridgeConnectionTest, TransformRowsSurviveDeselectAllClicksAndAFilter) {
  PjBridgeDialog dialog(testOptions());
  connectDialog(dialog, typedTopicsResponse({{"/tf", kTf}, {"/imu", kFloat}}));
  ASSERT_TRUE(dialog.onClicked("btnDeselectAll"));
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf"}));
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/tf"}));

  // The host reports only selectable rows: a selection omitting /tf does not drop it.
  ASSERT_TRUE(dialog.onSelectionChanged("topicsList", {"/imu"}));
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf", "/imu"}));
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/tf", "/imu"}));
  ASSERT_TRUE(dialog.onSelectionChanged("topicsList", {}));
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf"}));

  dialog.onTextChanged("lineEditFilter", "imu");
  EXPECT_NE(rowOf(table(dialog.widget_data(), "topicsList"), "/tf"), -1);  // a filter never hides it
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf"}));
}

TEST(PjBridgeConnectionTest, ASavedSelectionOmittingTransformsStillShowsThem) {
  PjBridgeDialog dialog(testOptions());
  ASSERT_TRUE(dialog.loadConfig(R"({"url":"ws://robot:9090","topics":[{"name":"/imu"}]})"));
  connectDialog(dialog, typedTopicsResponse({{"/tf", kTf}, {"/imu", kFloat}}));
  EXPECT_EQ(shownSelection(table(dialog.widget_data(), "topicsList")), (std::set<std::string>{"/tf", "/imu"}));
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/imu", "/tf"}));
}

TEST(PjBridgeConnectionTest, ALoadedSelectionKeepsItsTransforms) {
  PjBridgeDialog dialog(testOptions());
  ASSERT_TRUE(dialog.loadConfig(
      R"({"url":"ws://robot:9090","topics":[{"name":"/imu"},{"name":"/tf","schema_name":"tf2_msgs/msg/TFMessage"}]})"));
  EXPECT_EQ(savedTopics(dialog), (std::set<std::string>{"/imu", "/tf"}));
}

TEST(PjBridgeConnectionTest, ASavedSelectionOfTransformsOnlyStillAdvertisesEverything) {
  BridgeFixture fixture;
  ASSERT_TRUE(fixture.source.loadConfig(
      R"({"url":"ws://robot:9871","topics":[{"name":"/tf","schema_name":"tf2_msgs/msg/TFMessage"}]})"));
  ASSERT_TRUE(fixture.source.start());
  FakeSocket& socket = fixture.onlySocket();
  socket.acceptConnection();
  socket.deliverText(typedTopicsResponse({{"/imu", kFloat}, {"/odom", kFloat}, {"/tf", kTf}}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/imu", "/odom", "/tf"}));
}

TEST(PjBridgeConnectionTest, ASavedDialogSelectionCarriesTheTransformsToALegacyHost) {
  std::string saved;
  {
    PjBridgeDialog dialog(testOptions());
    connectDialog(dialog, typedTopicsResponse({{"/imu", kFloat}, {"/tf", kTf}}));
    ASSERT_TRUE(dialog.onSelectionChanged("topicsList", {"/imu"}));
    saved = dialog.saveConfig();
  }
  BridgeFixture fixture(/*demand_capable=*/false);
  ASSERT_TRUE(fixture.source.loadConfig(saved));
  ASSERT_TRUE(fixture.source.start());
  FakeSocket& socket = fixture.onlySocket();
  socket.acceptConnection();
  ASSERT_TRUE(fixture.source.poll());
  const auto subscribe = lastSent(socket);
  EXPECT_EQ(subscribe.value("command", std::string{}), "subscribe");
  EXPECT_EQ(subscribe.at("topics"), nlohmann::json::array({"/imu", "/tf"}));
}

TEST(PjBridgeConnectionTest, ATfNameOfAnotherTypeIsNotForced) {
  PjBridgeDialog dialog(testOptions());
  connectDialog(dialog, typedTopicsResponse({{"/tf", "std_msgs/msg/String"}, {"tf", kFloat}}));
  const auto tbl = table(dialog.widget_data(), "topicsList");
  EXPECT_TRUE(tbl.value("disabled_rows", std::set<int>{}).empty());
  EXPECT_TRUE(shownSelection(table(dialog.widget_data(), "topicsList")).empty());
}

TEST(PjBridgeConnectionTest, AdvertisedTransformsAreSubscribedWithNothingSelected) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");
  socket.acceptConnection();
  socket.deliverText(typedTopicsResponse({{"/imu", kFloat}, {"/tf", kTf}, {"/tf_static", kTf}, {"/fake_tf", kFloat}}));
  ASSERT_TRUE(fixture.source.poll());
  const auto subscribe = lastSent(socket);
  EXPECT_EQ(subscribe.value("command", std::string{}), "subscribe");
  EXPECT_EQ(subscribe.at("topics"), nlohmann::json::array({"/tf", "/tf_static"}));

  // The host's desired set never drops them.
  fixture.setActiveTopics({"/imu"});
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(lastSent(socket).at("topics"), nlohmann::json::array({"/imu"}));
  fixture.setActiveTopics({});
  ASSERT_TRUE(fixture.source.poll());
  const auto unsubscribe = lastSent(socket);
  EXPECT_EQ(unsubscribe.value("command", std::string{}), "unsubscribe");
  EXPECT_EQ(unsubscribe.at("topics"), nlohmann::json::array({"/imu"}));
}

TEST(PjBridgeConnectionTest, TransformsPassASavedAdvertiseFilter) {
  BridgeFixture fixture;
  ASSERT_TRUE(fixture.source.loadConfig(R"({"url":"ws://robot:9871","topics":[{"name":"/imu"}]})"));
  ASSERT_TRUE(fixture.source.start());
  FakeSocket& socket = fixture.onlySocket();
  socket.acceptConnection();
  socket.deliverText(typedTopicsResponse({{"/imu", kFloat}, {"/odom", kFloat}, {"/tf", kTf}}));
  ASSERT_TRUE(fixture.source.poll());
  ASSERT_FALSE(fixture.host.advertisements.empty());
  EXPECT_EQ(fixture.host.advertisements.back(), (std::vector<std::string>{"/imu", "/tf"}));
  EXPECT_EQ(lastSent(socket).at("topics"), nlohmann::json::array({"/tf"}));
}

TEST(PjBridgeConnectionTest, AbsentTransformsAreNotInvented) {
  BridgeFixture fixture;
  FakeSocket& socket = fixture.startWithUrl("ws://robot:9871");
  socket.acceptConnection();
  socket.deliverText(typedTopicsResponse({{"/imu", kFloat}}));
  ASSERT_TRUE(fixture.source.poll());
  EXPECT_EQ(commands(socket), (std::vector<std::string>{"subscribe_topic_updates", "get_topics"}));
}

TEST(PjBridgeConnectionTest, LegacyHostSubscribesTheDialogsTransformsToo) {
  BridgeFixture fixture(/*demand_capable=*/false);
  PjBridgeDialog& dialog = fixture.dialog();
  connectDialog(dialog, typedTopicsResponse({{"/imu", kFloat}, {"/tf", kTf}}));
  ASSERT_TRUE(dialog.onSelectionChanged("topicsList", {"/imu"}));
  ASSERT_TRUE(fixture.source.start());
  ASSERT_TRUE(fixture.source.poll());
  FakeSocket& socket = *fakeSockets().back();
  const auto subscribe = lastSent(socket);
  EXPECT_EQ(subscribe.value("command", std::string{}), "subscribe");
  EXPECT_EQ(subscribe.at("topics"), nlohmann::json::array({"/imu", "/tf"}));
}

}  // namespace
