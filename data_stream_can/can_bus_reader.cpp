// The only translation unit of data_stream_can that includes Qt.

#include "can_bus_reader.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

#include "pj_base/sdk/platform.hpp"
#include "pj_plugins/sdk/streaming_source.hpp"
#include "timestamp_anchor.hpp"

// Qt after the SDK headers (and QT_NO_KEYWORDS, see CMakeLists.txt): Qt's keyword macros must not
// reach them.
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtCore/QVariant>
#include <QtSerialBus/QCanBus>
#include <QtSerialBus/QCanBusDevice>
#include <QtSerialBus/QCanBusDeviceInfo>
#include <QtSerialBus/QCanBusFrame>

namespace can_stream {
namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(5);

// QCanBus is not thread-safe (unsynchronized lazy singleton and plugin map), and it is reached from
// the dialog's thread (availableBackends / availableInterfaces) and from reader threads
// (createDevice). Every QCanBus call holds this mutex.
std::mutex g_can_bus_mutex;

const int kModuleAnchor = 0;  // any address inside this module, for getSharedLibDir()

// Caller holds g_can_bus_mutex.
QCanBus* canBus() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Before QCanBus scans for backends: the bundled canbus/ plugin dir sits next to this module.
    const auto dir = PJ::sdk::getSharedLibDir(&kModuleAnchor);
    if (!dir.empty()) {
      QCoreApplication::addLibraryPath(QString::fromStdWString(dir.wstring()));
    }
  });
  return QCanBus::instance();
}

std::int64_t hostNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// VirtualCAN's server is one process-wide Qt object that lives in the thread that FIRST opens a
// virtualcan device, and serves only while that thread runs an event loop. Left to a reader thread
// it would die with that session, and every later session would connect but receive nothing. So it
// is started on the application thread, the one thread with an event loop that outlives every reader:
// opening a device there starts the server synchronously (VirtualCanBackend::open()), and the device
// itself is dropped at once, before its socket registers on a channel (kept, it would buffer that
// channel's traffic forever, unread). Run for every virtualcan session, not once: starting a running
// server is a no-op, and this also retries a listen() that failed earlier (port briefly taken).
void startVirtualCanServerOnAppThread(const QString& interface_name) {
  auto open_and_drop = [interface_name] {
    std::unique_ptr<QCanBusDevice> device;
    {
      std::lock_guard lock(g_can_bus_mutex);
      device.reset(canBus()->createDevice(QStringLiteral("virtualcan"), interface_name));
    }
    if (device) {
      device->connectDevice();
    }
  };
  QCoreApplication* app = QCoreApplication::instance();
  if (QThread::currentThread() == app->thread()) {
    open_and_drop();
  } else {
    // The caller is not the application thread, so blocking on it cannot deadlock on ourselves.
    QMetaObject::invokeMethod(app, open_and_drop, Qt::BlockingQueuedConnection);
  }
}

}  // namespace

std::vector<std::string> availableBackends() {
  std::vector<std::string> backends;
  std::lock_guard lock(g_can_bus_mutex);
  for (const QString& plugin : canBus()->plugins()) {
    backends.push_back(plugin.toStdString());
  }
  return backends;
}

PJ::Expected<std::vector<std::string>> availableInterfaces(const std::string& backend) {
  QString error;
  QList<QCanBusDeviceInfo> devices;
  {
    std::lock_guard lock(g_can_bus_mutex);
    devices = canBus()->availableDevices(QString::fromStdString(backend), &error);
  }
  if (devices.isEmpty() && !error.isEmpty()) {
    return PJ::unexpected(backend + ": " + error.toStdString());
  }
  std::vector<std::string> names;
  for (const QCanBusDeviceInfo& info : devices) {
    names.push_back(info.name().toStdString());
  }
  return names;
}

struct CanBusReader::Impl {
  std::unique_ptr<QThread> thread;  // controlled from the start()/stop() thread only
  // Set exactly once per session by the reader thread: nullopt once connected, else the error.
  std::promise<std::optional<std::string>> handshake;

  PJ::sdk::DrainQueue<RawCanFrame> frames;
  PJ::sdk::LatestValueSlot<std::string> fatal_error;
  std::atomic<std::uint64_t> error_frames{0};
  std::atomic<std::uint64_t> remote_frames{0};

  void run(const BusSettings& settings);
};

// The reader thread's body. Every QObject it touches (the device and the callbacks' context) is
// created, used and destroyed here; only the thread-safe members above are shared.
void CanBusReader::Impl::run(const BusSettings& settings) {
  const QString prefix = QString::fromStdString(settings.backend + "/" + settings.interface_name + ": ");
  bool handshake_done = false;
  bool was_connected = false;
  bool fatal_reported = false;
  // Before the handshake an error fails start(); after it, it is the session's one fatal error.
  auto report = [&](const QString& message) {
    if (!handshake_done) {
      handshake_done = true;
      handshake.set_value(message.toStdString());
    } else if (was_connected && !fatal_reported) {
      fatal_reported = true;
      fatal_error.set(message.toStdString());
    }
  };

  QString error;
  std::unique_ptr<QCanBusDevice> device;
  {
    std::lock_guard lock(g_can_bus_mutex);
    device.reset(
        canBus()->createDevice(
            QString::fromStdString(settings.backend), QString::fromStdString(settings.interface_name), &error));
  }
  if (!device) {
    report(prefix + error);
    return;
  }
  // CanFdKey first: PEAK validates the bitrate against it (nominal vs. classic bitrate table).
  if (settings.can_fd) {
    device->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(true));
  }
  if (settings.bitrate > 0) {
    device->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant(settings.bitrate));
  }
  if (settings.can_fd && settings.data_bitrate > 0) {
    device->setConfigurationParameter(QCanBusDevice::DataBitRateKey, QVariant(settings.data_bitrate));
  }
  // A rejected setting (ConfigurationError, e.g. a bitrate the adapter does not support) or a device
  // the backend refused to set up (e.g. a malformed interface name) must not stream with other
  // settings than the user asked for.
  if (device->error() != QCanBusDevice::NoError) {
    report(prefix + device->errorString());
    return;
  }

  // Receiver of every callback below. Declared after `device`, so it is destroyed first and no
  // callback can run while the device tears down.
  QObject context;
  TimestampAnchor anchor;
  QObject::connect(device.get(), &QCanBusDevice::framesReceived, &context, [&] {
    for (const QCanBusFrame& frame : device->readAllFrames()) {
      const auto type = frame.frameType();
      if (type == QCanBusFrame::ErrorFrame) {
        error_frames.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (type == QCanBusFrame::RemoteRequestFrame) {
        remote_frames.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (type != QCanBusFrame::DataFrame) {
        continue;
      }
      RawCanFrame raw;
      const std::int64_t now = hostNowNs();
      if (settings.timestamps == TimestampSource::kHost) {
        raw.ts_ns = now;
      } else {
        const auto stamp = frame.timeStamp();
        raw.ts_ns = anchor.map(stamp.seconds() * 1'000'000'000 + stamp.microSeconds() * 1'000, now);
      }
      raw.can_id = frame.frameId();
      raw.extended = frame.hasExtendedFrameFormat();
      const QByteArray payload = frame.payload();
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.constData());
      raw.data.assign(bytes, bytes + payload.size());
      frames.push(std::move(raw));
    }
  });
  QObject::connect(device.get(), &QCanBusDevice::errorOccurred, &context, [&](QCanBusDevice::CanBusError code) {
    // Fatal: the link is gone (PEAK reports an unplug as a failed CAN_Read, ReadError, without
    // any state change). Other errors (WriteError, ConfigurationError at runtime, ...) are not.
    const bool fatal = code == QCanBusDevice::ConnectionError || code == QCanBusDevice::ReadError;
    if (fatal) {
      report(prefix + device->errorString());
    }
  });
  QObject::connect(device.get(), &QCanBusDevice::stateChanged, &context, [&](QCanBusDevice::CanBusDeviceState state) {
    if (state == QCanBusDevice::ConnectedState) {
      was_connected = true;
      if (!handshake_done) {
        handshake_done = true;
        handshake.set_value(std::nullopt);
      }
    } else if (was_connected) {
      const QString reason = device->errorString();
      report(prefix + (reason.isEmpty() ? QStringLiteral("device disconnected") : reason));
    } else if (state == QCanBusDevice::UnconnectedState) {
      const QString reason = device->errorString();
      report(prefix + (reason.isEmpty() ? QStringLiteral("connection failed") : reason));
    }
  });

  if (!device->connectDevice()) {
    report(prefix + device->errorString());  // no-op if stateChanged already reported
    return;
  }

  // Until stop() quits this thread. A quit() that lands before exec() makes it return at once.
  QEventLoop loop;
  loop.exec();

  QObject::disconnect(device.get(), nullptr, &context, nullptr);
  const auto state = device->state();
  if (state == QCanBusDevice::ConnectingState || state == QCanBusDevice::ConnectedState) {
    device->disconnectDevice();
  }
}

CanBusReader::CanBusReader() : impl_(std::make_unique<Impl>()) {}

CanBusReader::~CanBusReader() {
  stop();
}

PJ::Status CanBusReader::start(const BusSettings& settings) {
  stop();
  if (QCoreApplication::instance() == nullptr) {
    return PJ::unexpected("CAN streaming needs PlotJuggler's Qt application (no QCoreApplication in this process)");
  }
  impl_->frames.clear();
  (void)impl_->fatal_error.take();
  impl_->error_frames = 0;
  impl_->remote_frames = 0;
  impl_->handshake = {};
  auto handshake = impl_->handshake.get_future();
  if (settings.backend == "virtualcan") {
    startVirtualCanServerOnAppThread(QString::fromStdString(settings.interface_name));
  }

  // The QThread object belongs to this (controlling) thread; everything created inside run() lives
  // on the new thread.
  impl_->thread.reset(QThread::create([impl = impl_.get(), settings] { impl->run(settings); }));
  impl_->thread->setObjectName(QStringLiteral("can_reader"));
  impl_->thread->start();

  if (handshake.wait_for(kConnectTimeout) != std::future_status::ready) {
    stop();
    return PJ::unexpected(
        settings.backend + "/" + settings.interface_name + ": timed out waiting for the device to connect");
  }
  if (auto error = handshake.get()) {
    stop();
    return PJ::unexpected(std::move(*error));
  }
  return PJ::okStatus();
}

void CanBusReader::stop() {
  if (!impl_->thread) {
    return;
  }
  impl_->thread->quit();
  impl_->thread->wait();  // run() has returned: the device is destroyed, no callback is pending
  impl_->thread.reset();
}

std::queue<RawCanFrame> CanBusReader::drainFrames() {
  return impl_->frames.drain();
}

std::optional<std::string> CanBusReader::takeFatalError() {
  return impl_->fatal_error.take();
}

ReaderCounts CanBusReader::counts() const {
  return {impl_->error_frames.load(std::memory_order_relaxed), impl_->remote_frames.load(std::memory_order_relaxed)};
}

}  // namespace can_stream
