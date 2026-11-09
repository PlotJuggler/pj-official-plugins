#include "can_bus_reader.hpp"

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QVariant>
#include <QtSerialBus/QCanBus>
#include <QtSerialBus/QCanBusDevice>
#include <QtSerialBus/QCanBusFrame>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Every test runs in its own process (gtest_discover_tests): VirtualCAN's server is a process-wide
// Qt global, and a test must see it the way a fresh PlotJuggler process does. The test thread is the
// QCoreApplication thread, where CanBusReader starts that server; waitFor() pumps it.

namespace {

using can_stream::CanBusReader;
using can_stream::RawCanFrame;
using namespace std::chrono_literals;

constexpr const char* kServerFlag = "--virtualcan-server";
// The server-loss test hosts its VirtualCAN server in a child process on this port: the in-process
// server can never go away.
constexpr const char* kChildServerInterface = "tcp://127.0.0.1:35469/can0";
constexpr std::uint32_t kProbeId = 0x7FF;

// Pumps this thread's events until pred() holds.
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

// The server forwards a frame only to clients whose "connect:canN" it has already processed: send
// probes until one reaches the reader, so the frames after it cannot be dropped.
bool waitForLink(QCanBusDevice& writer, CanBusReader& reader) {
  const QCanBusFrame probe(kProbeId, QByteArray(1, '\0'));
  return waitFor([&] {
    writer.writeFrame(probe);
    return !reader.drainFrames().empty();
  });
}

// The next `count` non-probe frames (probes still in flight are skipped).
std::vector<RawCanFrame> receive(CanBusReader& reader, std::size_t count) {
  std::vector<RawCanFrame> frames;
  waitFor([&] {
    for (auto batch = reader.drainFrames(); !batch.empty(); batch.pop()) {
      if (batch.front().can_id != kProbeId) {
        frames.push_back(std::move(batch.front()));
      }
    }
    return frames.size() >= count;
  });
  return frames;
}

TEST(CanBusReader, ReceivesFramesOverVirtualCan) {
  CanBusReader reader;
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface_name = "can0", .can_fd = true}));

  auto writer = connectedWriter(QStringLiteral("can0"));
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(waitForLink(*writer, reader));

  QCanBusFrame standard(0x123, QByteArray("\x01\x02\x03", 3));
  QCanBusFrame extended(0x18FF50E5, QByteArray(8, '\x5A'));
  extended.setExtendedFrameFormat(true);
  QCanBusFrame fd(0x456, QByteArray(64, '\x11'));
  fd.setFlexibleDataRateFormat(true);
  ASSERT_TRUE(writer->writeFrame(standard));
  ASSERT_TRUE(writer->writeFrame(extended));
  ASSERT_TRUE(writer->writeFrame(fd));

  const auto frames = receive(reader, 3);
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

// The first virtualcan session is started from another thread, as the host does, and the VirtualCAN
// server must still outlive it: a server born in that session's reader thread would stop forwarding
// when the session stops.
TEST(CanBusReader, VirtualCanSurvivesReaderRestart) {
  CanBusReader first;
  std::atomic<int> phase{0};  // 1: start() returned, 2: stop requested, 3: stopped
  bool first_started = false;
  std::thread controller([&] {
    first_started = static_cast<bool>(first.start({.backend = "virtualcan", .interface_name = "can0"}));
    phase = 1;
    while (phase != 2) {
      std::this_thread::sleep_for(1ms);
    }
    first.stop();
    phase = 3;
  });
  EXPECT_TRUE(waitFor([&] { return phase >= 1; }));  // start() hops onto this thread: keep pumping
  EXPECT_TRUE(first_started);

  auto writer = connectedWriter(QStringLiteral("can0"));
  const QCanBusFrame frame(0x321, QByteArray(2, '\x07'));
  if (first_started && writer) {
    EXPECT_TRUE(waitForLink(*writer, first));
    writer->writeFrame(frame);
    EXPECT_EQ(receive(first, 1).size(), 1U);
  }
  phase = 2;
  EXPECT_TRUE(waitFor([&] { return phase == 3; }));
  controller.join();
  ASSERT_NE(writer, nullptr);

  CanBusReader second;
  ASSERT_TRUE(second.start({.backend = "virtualcan", .interface_name = "can0"}));
  ASSERT_TRUE(waitForLink(*writer, second)) << "the VirtualCAN server died with the first session";
  writer->writeFrame(frame);
  const auto frames = receive(second, 1);
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].can_id, 0x321U);
}

// The host's contract: start() on the GUI (QCoreApplication) thread, then stop() and the
// destruction on its session worker thread.
TEST(CanBusReader, StopFromAnotherThread) {
  auto reader = std::make_unique<CanBusReader>();
  ASSERT_TRUE(reader->start({.backend = "virtualcan", .interface_name = "can0"}));
  auto writer = connectedWriter(QStringLiteral("can0"));
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(waitForLink(*writer, *reader));
  const QCanBusFrame frame(0x222, QByteArray(3, '\x09'));
  writer->writeFrame(frame);
  ASSERT_EQ(receive(*reader, 1).size(), 1U);

  std::atomic<bool> done{false};
  std::thread worker([&] {
    reader->stop();
    reader.reset();
    done = true;
  });
  EXPECT_TRUE(waitFor([&] { return done.load(); }));
  worker.join();
  QCoreApplication::processEvents();  // anything wrongly left posted for the dead reader runs here

  CanBusReader next;
  ASSERT_TRUE(next.start({.backend = "virtualcan", .interface_name = "can0"}));
  ASSERT_TRUE(waitForLink(*writer, next));
  writer->writeFrame(frame);
  const auto frames = receive(next, 1);
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].can_id, 0x222U);
}

TEST(CanBusReader, UnknownBackendFailsCleanly) {
  CanBusReader reader;
  auto status = reader.start({.backend = "nosuchbackend", .interface_name = "can0"});
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("nosuchbackend"), std::string::npos) << status.error();
  reader.stop();
}

// Qt's socketcan backend cannot set a rate on a running interface and only logs the failure: asking
// for one must fail the start, not stream at the interface's own rate. Rejected before any socket opens.
TEST(CanBusReader, SocketCanBitrateFailsTheStart) {
  CanBusReader reader;
  auto status = reader.start({.backend = "socketcan", .interface_name = "can0", .bitrate = 500000});
  ASSERT_FALSE(status);
  EXPECT_NE(status.error().find("socketcan/can0: "), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("ip link set can0 type can bitrate"), std::string::npos) << status.error();
  status = reader.start({.backend = "socketcan", .interface_name = "can0", .can_fd = true, .data_bitrate = 2000000});
  EXPECT_FALSE(status);
}

TEST(CanBusReader, MissingVendorDriverFailsCleanly) {
  const auto backends = can_stream::availableBackends();
  if (std::find(backends.begin(), backends.end(), "peakcan") == backends.end()) {
    GTEST_SKIP() << "the peakcan backend is not installed";
  }
  CanBusReader reader;
  auto status = reader.start({.backend = "peakcan", .interface_name = "usb0"});
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
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface_name = kChildServerInterface}));
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
  ASSERT_TRUE(reader.start({.backend = "virtualcan", .interface_name = "can0"}));
  reader.stop();
  reader.stop();
  EXPECT_TRUE(reader.drainFrames().empty());
}

// Child-process mode for ServerLossIsReported: host a VirtualCAN server until killed.
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
