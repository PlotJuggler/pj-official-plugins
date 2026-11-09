// End-to-end: the BUILT plugin, loaded and driven the way PlotJuggler does (bind -> loadConfig ->
// start -> poll... -> stop), receiving frames from a test-side VirtualCAN writer. Every test runs in its
// own process (gtest_discover_tests): VirtualCAN's server is a process-wide Qt global on a fixed TCP
// port. The test thread is the QCoreApplication thread, as the host's GUI thread is.

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QVariant>
#include <QtSerialBus/QCanBus>
#include <QtSerialBus/QCanBusDevice>
#include <QtSerialBus/QCanBusFrame>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "bus_name.hpp"
#include "source_import_harness.hpp"

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using pj_can_dbc::testing::generic;
using pj_can_dbc::testing::ScratchDir;
using pj_can_dbc::testing::writeEngineDbc;
using pj_can_dbc::testing::detail::CaptureHost;

constexpr const char* kServerFlag = "--virtualcan-server";
// FatalReaderErrorStopsStream hosts its VirtualCAN server in a child process on this port: the in-process
// server can never go away.
constexpr const char* kChildServerInterface = "tcp://127.0.0.1:35469/can0";

struct Session {
  PJ::DataSourceLibrary library;
  CaptureHost host;
  std::optional<PJ::DataSourceHandle> handle;
  std::string start_error;
  bool started = false;
};

// Loads the plugin, binds the capture host, loads `config` and starts it.
std::unique_ptr<Session> startPlugin(const std::string& config) {
  auto library = PJ::DataSourceLibrary::load(CAN_STREAM_PLUGIN_PATH);
  if (!library) {
    ADD_FAILURE() << "load: " << library.error();
    return nullptr;
  }
  auto session = std::unique_ptr<Session>(new Session{std::move(*library), {}, {}, {}, false});
  session->handle.emplace(session->library.createHandle());
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SourceWriteHostService>(session->host.writeHost());
  registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(session->host.runtimeHost());
  if (auto bound = session->handle->bind(registry.view()); !bound) {
    ADD_FAILURE() << "bind: " << bound.error();
    return nullptr;
  }
  if (auto loaded = session->handle->loadConfig(config); !loaded) {
    ADD_FAILURE() << "loadConfig: " << loaded.error();
    return nullptr;
  }
  auto started = session->handle->start();
  session->started = static_cast<bool>(started);
  if (!started) {
    session->start_error = started.error();
  }
  return session;
}

std::unique_ptr<QCanBusDevice> connectedWriter() {
  QString error;
  std::unique_ptr<QCanBusDevice> device(
      QCanBus::instance()->createDevice(QStringLiteral("virtualcan"), QStringLiteral("can0"), &error));
  if (!device || !device->connectDevice()) {
    ADD_FAILURE() << (device ? device->errorString() : error).toStdString();
    return nullptr;
  }
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (device->state() != QCanBusDevice::ConnectedState && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(device->state(), QCanBusDevice::ConnectedState);
  return device;
}

std::uint64_t rowsOf(const CaptureHost& host, const std::string& topic) {
  const auto it = host.rows.find(topic);
  return it == host.rows.end() ? 0 : it->second;
}

int countMessages(const CaptureHost& host, PJ_data_source_message_level_t level, std::string_view text) {
  return static_cast<int>(std::count_if(host.messages.begin(), host.messages.end(), [&](const auto& message) {
    return message.level == static_cast<int>(level) && message.text.find(text) != std::string::npos;
  }));
}

// Resends `frames` and polls, pumping Qt events, until done() holds (5 s deadline). Resending covers the
// VirtualCAN server forwarding only to clients whose connection it has already processed.
bool sendUntil(
    Session& session, QCanBusDevice& writer, const std::vector<QCanBusFrame>& frames,
    const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    for (const auto& frame : frames) {
      writer.writeFrame(frame);
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(20ms);
    if (auto polled = session.handle->poll(); !polled) {
      ADD_FAILURE() << "poll: " << polled.error();
      return false;
    }
  }
  return true;
}

std::string configWith(const std::string& interface_name, const nlohmann::json& dictionaries) {
  return nlohmann::json{{"backend", "virtualcan"}, {"interface", interface_name}, {"dictionaries", dictionaries}}
      .dump();
}

TEST(CanSource, BusNameReplacesSlashAndWhitespace) {
  EXPECT_EQ(can_stream::busName("can0"), "can0");
  EXPECT_EQ(can_stream::busName("tcp://127.0.0.1:35468/can0"), "tcp:__127.0.0.1:35468_can0");
  EXPECT_EQ(can_stream::busName("my bus"), "my_bus");
  EXPECT_EQ(can_stream::busName("a\tb\nc"), "a_b_c");
}

TEST(CanSource, DecodesVirtualCanTrafficWithDbc) {
  ScratchDir scratch;
  const auto dbc = scratch.root() / "vehicle.dbc";
  writeEngineDbc(dbc, "Engine", 8);
  auto session = startPlugin(configWith("can0", {generic(dbc)}));
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->started) << session->start_error;
  auto writer = connectedWriter();
  ASSERT_NE(writer, nullptr);

  const QCanBusFrame engine(256, QByteArray(8, '\x10'));
  const QCanBusFrame unknown(0x7FF, QByteArray(2, '\x01'));
  const auto& host = session->host;
  EXPECT_TRUE(sendUntil(*session, *writer, {engine, unknown}, [&] {
    return rowsOf(host, "CAN/can0/Engine") >= 1 && rowsOf(host, "CAN/can0/0x7FF") >= 1;
  }));
  session->handle->stop();

  EXPECT_GE(rowsOf(host, "CAN/can0/Engine"), 1U);
  EXPECT_GE(rowsOf(host, "CAN/can0/0x7FF"), 1U);
  const auto fields = host.last_field_count.find("CAN/can0/0x7FF");
  ASSERT_NE(fields, host.last_field_count.end());
  EXPECT_EQ(fields->second, 2U);  // byte0, byte1
  EXPECT_EQ(countMessages(host, PJ_DATA_SOURCE_MESSAGE_INFO, "decoded"), 1);
}

TEST(CanSource, StartFailsWithReadableError) {
  auto session = startPlugin(R"({"backend":"nosuchbackend","interface":"can0"})");
  ASSERT_NE(session, nullptr);
  EXPECT_FALSE(session->started);
  EXPECT_NE(session->start_error.find("nosuchbackend"), std::string::npos) << session->start_error;
  session->handle->stop();  // safe after a failed start
}

TEST(CanSource, MissingDictionaryIsAWarningNotAFailure) {
  auto session = startPlugin(configWith("can0", {"/no/such/dir/gone.dbc"}));
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->started) << session->start_error;
  EXPECT_EQ(countMessages(session->host, PJ_DATA_SOURCE_MESSAGE_WARNING, "gone.dbc"), 1);
  EXPECT_EQ(
      countMessages(
          session->host, PJ_DATA_SOURCE_MESSAGE_WARNING,
          "no CAN message loaded from the dictionaries; nothing will be decoded"),
      1);
  session->handle->stop();
}

void writeDbc(const fs::path& file, const std::string& messages) {
  std::ofstream out(file, std::ios::binary);
  out << "VERSION \"1.0.0\"\n\nNS_ :\n\nBS_:\n\nBU_: ECU\n\n" << messages;
}

// One plugin instance, two sessions with different dictionaries naming the same id: the second session
// must decode with its own dictionary only (decoder rebuilt, router topic cache reset). id 512 is only in
// the first dictionary, so in the second session it must fall back to raw.
TEST(CanSource, RestartsInSameInstance) {
  ScratchDir scratch;
  const auto old_dbc = scratch.root() / "old.dbc";
  const auto new_dbc = scratch.root() / "new.dbc";
  writeDbc(
      old_dbc,
      "BO_ 256 OldName: 8 ECU\n SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" ECU\n\n"
      "BO_ 512 OldOnly: 8 ECU\n SG_ Rpm : 0|16@1+ (1,0) [0|65535] \"rpm\" ECU\n");
  writeDbc(new_dbc, "BO_ 256 NewName: 8 ECU\n SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" ECU\n");

  auto session = startPlugin(configWith("can0", {generic(old_dbc)}));
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->started) << session->start_error;
  auto writer = connectedWriter();
  ASSERT_NE(writer, nullptr);
  const QCanBusFrame shared_id(256, QByteArray(8, '\x22'));
  const QCanBusFrame old_only(512, QByteArray(8, '\x33'));
  const auto& host = session->host;
  ASSERT_TRUE(sendUntil(*session, *writer, {shared_id, old_only}, [&] {
    return rowsOf(host, "CAN/can0/OldName") >= 1 && rowsOf(host, "CAN/can0/OldOnly") >= 1;
  }));
  session->handle->stop();
  const auto old_only_rows = rowsOf(host, "CAN/can0/OldOnly");

  ASSERT_TRUE(session->handle->loadConfig(configWith("can0", {generic(new_dbc)})));
  auto restarted = session->handle->start();
  ASSERT_TRUE(restarted) << restarted.error();
  EXPECT_TRUE(sendUntil(*session, *writer, {shared_id, old_only}, [&] {
    return rowsOf(host, "CAN/can0/NewName") >= 1 && rowsOf(host, "CAN/can0/0x200") >= 1;
  }));
  session->handle->stop();

  EXPECT_GE(rowsOf(host, "CAN/can0/NewName"), 1U);
  EXPECT_GE(rowsOf(host, "CAN/can0/0x200"), 1U);
  EXPECT_EQ(rowsOf(host, "CAN/can0/OldOnly"), old_only_rows);                 // the first dictionary is gone
  EXPECT_EQ(countMessages(host, PJ_DATA_SOURCE_MESSAGE_INFO, "decoded"), 2);  // one summary per session
}

// The bus disappearing mid-stream (adapter unplugged): the VirtualCAN server is hosted by a child process,
// which is killed under the running session.
TEST(CanSource, FatalReaderErrorStopsStream) {
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  server.start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(kServerFlag)});
  ASSERT_TRUE(server.waitForStarted(5000));
  const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
  while (!server.peek(64).contains("ready") && std::chrono::steady_clock::now() < ready_deadline) {
    server.waitForReadyRead(50);
  }
  ASSERT_TRUE(server.peek(64).contains("ready")) << "the VirtualCAN server process did not come up";

  auto session = startPlugin(configWith(kChildServerInterface, nlohmann::json::array()));
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->started) << session->start_error;
  ASSERT_TRUE(session->handle->poll());

  server.kill();
  ASSERT_TRUE(server.waitForFinished(5000));

  PJ::Status polled = PJ::okStatus();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (polled && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(10ms);
    polled = session->handle->poll();
  }
  ASSERT_FALSE(polled) << "poll() never reported the lost bus";
  EXPECT_FALSE(polled.error().empty());
  EXPECT_TRUE(session->handle->poll());  // the error is returned once, not on every later poll
  session->handle->stop();
  // Only returned: the host shows a failed poll() itself, a reportMessage(kError) would show it twice.
  EXPECT_EQ(countMessages(session->host, PJ_DATA_SOURCE_MESSAGE_ERROR, ""), 0);
}

// Child-process mode for FatalReaderErrorStopsStream: host a VirtualCAN server until killed.
int runVirtualCanServer(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  std::unique_ptr<QCanBusDevice> device(
      QCanBus::instance()->createDevice(QStringLiteral("virtualcan"), QString::fromLatin1(kChildServerInterface)));
  if (!device) {
    return 1;
  }
  QObject::connect(device.get(), &QCanBusDevice::stateChanged, [](QCanBusDevice::CanBusDeviceState state) {
    if (state == QCanBusDevice::ConnectedState) {
      std::fputs("ready\n", stdout);
      std::fflush(stdout);
    }
  });
  if (!device->connectDevice()) {
    return 1;
  }
  return QCoreApplication::exec();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], kServerFlag) == 0) {
    return runVirtualCanServer(argc, argv);
  }
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
