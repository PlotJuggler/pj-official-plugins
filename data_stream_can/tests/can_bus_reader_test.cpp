#include "can_bus_reader.hpp"

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
#include <memory>
#include <thread>
#include <vector>

namespace {

using can_stream::BusSettings;
using can_stream::CanBusReader;
using can_stream::RawCanFrame;
using namespace std::chrono_literals;

constexpr const char* kServerFlag = "--virtualcan-server";
// The server-loss test hosts its VirtualCAN server in a child process on this port (the in-process
// server, on the default 35468, can never go away: see main()).
constexpr const char* kChildServerInterface = "tcp://127.0.0.1:35469/can0";

// Pumps this thread's events (the in-process VirtualCAN server lives here) until pred() holds.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

std::unique_ptr<QCanBusDevice> connectedWriter(const QString& interface_name) {
  QString error;
  std::unique_ptr<QCanBusDevice> device(
      QCanBus::instance()->createDevice(QStringLiteral("virtualcan"), interface_name, &error));
  if (!device) {
    ADD_FAILURE() << error.toStdString();
    return nullptr;
  }
  device->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(true));
  EXPECT_TRUE(device->connectDevice()) << device->errorString().toStdString();
  EXPECT_TRUE(waitFor([&] { return device->state() == QCanBusDevice::ConnectedState; }));
  return device;
}

TEST(CanBusReader, ReceivesFramesOverVirtualCan) {
  CanBusReader reader;
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface = "can0", .can_fd = true}));

  auto writer = connectedWriter(QStringLiteral("can0"));
  ASSERT_NE(writer, nullptr);
  // Let the server register both clients' "connect:can0" before the first frame goes through:
  // it forwards a frame only to clients already registered on that channel.
  waitFor([] { return false; }, 200ms);

  QCanBusFrame standard(0x123, QByteArray("\x01\x02\x03", 3));
  QCanBusFrame extended(0x18FF50E5, QByteArray(8, '\x5A'));
  extended.setExtendedFrameFormat(true);
  QCanBusFrame fd(0x456, QByteArray(64, '\x11'));
  fd.setFlexibleDataRateFormat(true);
  ASSERT_TRUE(writer->writeFrame(standard));
  ASSERT_TRUE(writer->writeFrame(extended));
  ASSERT_TRUE(writer->writeFrame(fd));

  std::vector<RawCanFrame> frames;
  ASSERT_TRUE(waitFor([&] {
    auto batch = reader.drainFrames();
    for (; !batch.empty(); batch.pop()) {
      frames.push_back(std::move(batch.front()));
    }
    return frames.size() >= 3;
  })) << "received "
      << frames.size() << " frames";
  ASSERT_EQ(frames.size(), 3U);

  EXPECT_EQ(frames[0].can_id, 0x123U);
  EXPECT_FALSE(frames[0].extended);
  EXPECT_EQ(frames[0].data, (std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_EQ(frames[1].can_id, 0x18FF50E5U);
  EXPECT_TRUE(frames[1].extended);
  EXPECT_EQ(frames[1].data.size(), 8U);
  EXPECT_EQ(frames[2].can_id, 0x456U);
  EXPECT_FALSE(frames[2].extended);
  EXPECT_EQ(frames[2].data.size(), 64U);
  for (const auto& frame : frames) {
    EXPECT_GT(frame.ts_ns, 1'600'000'000'000'000'000);
  }
  EXPECT_FALSE(reader.takeFatalError().has_value());
}

TEST(CanBusReader, UnknownBackendFailsCleanly) {
  CanBusReader reader;
  auto status = reader.start({.backend = "nosuchbackend", .interface = "can0"});
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("nosuchbackend"), std::string::npos) << status.error();
  reader.stop();
}

TEST(CanBusReader, MissingVendorDriverFailsCleanly) {
  const auto backends = can_stream::availableBackends();
  if (std::find(backends.begin(), backends.end(), "peakcan") == backends.end()) {
    GTEST_SKIP() << "the peakcan backend is not installed";
  }
  CanBusReader reader;
  auto status = reader.start({.backend = "peakcan", .interface = "usb0"});
  if (status) {
    GTEST_SKIP() << "a PEAK adapter is attached";
  }
  EXPECT_FALSE(status.error().empty());
  reader.stop();
  reader.stop();
}

// The adapter disappearing while streaming: the VirtualCAN server is hosted by a child process,
// which is killed under the connected reader.
TEST(CanBusReader, ServerLossIsReported) {
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  server.start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(kServerFlag)});
  ASSERT_TRUE(server.waitForStarted(5000));
  ASSERT_TRUE(waitFor([&] {
    server.waitForReadyRead(50);
    return server.peek(64).contains("ready");
  })) << "the VirtualCAN server process did not come up";

  CanBusReader reader;
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface = kChildServerInterface}));
  EXPECT_FALSE(reader.takeFatalError().has_value());

  server.kill();
  ASSERT_TRUE(server.waitForFinished(5000));

  std::optional<std::string> error;
  EXPECT_TRUE(waitFor([&] {
    error = reader.takeFatalError();
    return error.has_value();
  }));
  if (error) {
    EXPECT_FALSE(error->empty());
  }
  reader.stop();
}

TEST(CanBusReader, StopIsIdempotent) {
  { CanBusReader never_started; }
  CanBusReader reader;
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface = "can0"}));
  reader.stop();
  reader.stop();
  EXPECT_TRUE(reader.drainFrames().empty());
}

// Child-process mode for ServerLossIsReported: host a VirtualCAN server until killed.
int runVirtualCanServer(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  std::unique_ptr<QCanBusDevice> device(
      QCanBus::instance()->createDevice(QStringLiteral("virtualcan"), QString::fromLatin1(kChildServerInterface)));
  if (!device || !device->connectDevice()) {
    return 1;
  }
  QObject::connect(device.get(), &QCanBusDevice::stateChanged, [](QCanBusDevice::CanBusDeviceState state) {
    if (state == QCanBusDevice::ConnectedState) {
      std::fputs("ready\n", stdout);
      std::fflush(stdout);
    }
  });
  return QCoreApplication::exec();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], kServerFlag) == 0) {
    return runVirtualCanServer(argc, argv);
  }
  QCoreApplication app(argc, argv);
  // VirtualCAN's server is one process-wide object living in the thread that FIRST opened a
  // virtualcan device, and it never goes away. Open one here so it lives in this (test) thread,
  // whose wait loops pump it: a server born in a reader thread would stop forwarding once that
  // reader stops.
  {
    std::unique_ptr<QCanBusDevice> primer(
        QCanBus::instance()->createDevice(QStringLiteral("virtualcan"), QStringLiteral("can9")));
    if (!primer || !primer->connectDevice()) {
      std::fputs("cannot start the VirtualCAN server\n", stderr);
      return 1;
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
