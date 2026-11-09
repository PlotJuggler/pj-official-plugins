// End-to-end: the BUILT plugin, loaded and driven the way PlotJuggler does (bind -> loadConfig ->
// start -> poll... -> stop), receiving frames from a test-side VirtualCAN writer. Every test runs in its
// own process (gtest_discover_tests): VirtualCAN's server is a process-wide Qt global on a fixed TCP
// port. The test thread is the QCoreApplication thread, as the host's GUI thread is.

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QVariant>
#include <QtSerialBus/QCanBus>
#include <QtSerialBus/QCanBusDevice>
#include <QtSerialBus/QCanBusFrame>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "source_import_harness.hpp"

namespace {

using namespace std::chrono_literals;
using pj_can_dbc::testing::ScratchDir;
using pj_can_dbc::testing::detail::CaptureHost;

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

TEST(CanSource, DecodesVirtualCanTrafficWithDbc) {
  ScratchDir scratch;
  const auto dbc = scratch.root() / "vehicle.dbc";
  pj_can_dbc::testing::writeEngineDbc(dbc, "Engine", 8);
  const nlohmann::json config = {
      {"backend", "virtualcan"},
      {"interface", "can0"},
      {"dictionaries", {pj_can_dbc::testing::generic(dbc)}},
  };

  auto session = startPlugin(config.dump());
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->started) << session->start_error;
  auto writer = connectedWriter();
  ASSERT_NE(writer, nullptr);

  // The server forwards only to clients whose connection it has already processed: resend until both
  // frames have arrived.
  const QCanBusFrame engine(256, QByteArray(8, '\x10'));
  const QCanBusFrame unknown(0x7FF, QByteArray(2, '\x01'));
  auto& rows = session->host.rows;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!(rows["CAN/can0/Engine"] >= 1 && rows.count("CAN/can0/0x7FF") != 0) &&
         std::chrono::steady_clock::now() < deadline) {
    writer->writeFrame(engine);
    writer->writeFrame(unknown);
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(session->handle->poll());
  }
  session->handle->stop();

  EXPECT_GE(rows["CAN/can0/Engine"], 1U);
  EXPECT_EQ(rows.count("CAN/can0/0x7FF"), 1U);
  EXPECT_EQ(session->host.last_field_count["CAN/can0/0x7FF"], 2U);  // byte0, byte1
  bool summary = false;
  for (const auto& message : session->host.messages) {
    summary |= message.level == PJ_DATA_SOURCE_MESSAGE_INFO && message.text.find("decoded") != std::string::npos;
  }
  EXPECT_TRUE(summary);
}

TEST(CanSource, StartFailsWithReadableError) {
  auto session = startPlugin(R"({"backend":"nosuchbackend","interface":"can0"})");
  ASSERT_NE(session, nullptr);
  EXPECT_FALSE(session->started);
  EXPECT_NE(session->start_error.find("nosuchbackend"), std::string::npos) << session->start_error;
  session->handle->stop();  // safe after a failed start
}

TEST(CanSource, MissingDictionaryIsAWarningNotAFailure) {
  const nlohmann::json config = {
      {"backend", "virtualcan"}, {"interface", "can0"}, {"dictionaries", {"/no/such/dir/gone.dbc"}}};
  auto session = startPlugin(config.dump());
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->started) << session->start_error;
  bool warned = false;
  for (const auto& message : session->host.messages) {
    warned |= message.level == PJ_DATA_SOURCE_MESSAGE_WARNING && message.text.find("gone.dbc") != std::string::npos;
  }
  EXPECT_TRUE(warned);
  session->handle->stop();
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
