#pragma once

// Qt-free facade over QtSerialBus: can_bus_reader.cpp is the only translation unit that includes Qt.

#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"

namespace can_stream {

enum class TimestampSource { kDevice, kHost };

struct BusSettings {
  std::string backend;         // QCanBus plugin key: "socketcan", "peakcan", "virtualcan", ...
  std::string interface_name;  // backend-specific interface: "can0", "usb0", ... (UI/JSON key: "interface")
  int bitrate = 0;             // 0 = keep the device's setting
  bool can_fd = false;
  int data_bitrate = 0;  // 0 = keep the device's setting; only applied when can_fd
  TimestampSource timestamps = TimestampSource::kDevice;
};

struct RawCanFrame {
  std::int64_t ts_ns = 0;
  std::uint32_t can_id = 0;
  bool extended = false;
  std::vector<std::uint8_t> data;
};

struct ReaderCounts {
  std::uint64_t error_frames = 0;
  std::uint64_t remote_frames = 0;
};

/// QCanBus::plugins().
std::vector<std::string> availableBackends();
/// QCanBus::availableDevices(backend), by interface name.
PJ::Expected<std::vector<std::string>> availableInterfaces(const std::string& backend);

/// Reads one CAN interface on its own thread. The QCanBusDevice is created, connected, read and
/// destroyed on that thread; the frames, counters and the fatal error are handed over through
/// thread-safe containers, so drainFrames() / takeFatalError() / counts() can be called from any
/// thread while it runs. start(), stop() and the destructor may each run on a different thread
/// (PlotJuggler starts a source on its GUI thread, then polls, stops and destroys it on the
/// session's worker), but never concurrently with each other.
class CanBusReader {
 public:
  CanBusReader();
  ~CanBusReader();  // calls stop()
  CanBusReader(const CanBusReader&) = delete;
  CanBusReader& operator=(const CanBusReader&) = delete;

  /// Opens the device and waits for it to connect: up to 5 s for a backend that connects
  /// asynchronously, but a vendor driver that blocks inside its open call holds start() (and a
  /// stop() it triggers) for as long as that call takes. Stops a previous session first.
  /// For "virtualcan" it first starts the VirtualCAN server on the QCoreApplication thread, waiting
  /// up to 5 s for that thread to run it.
  PJ::Status start(const BusSettings& settings);
  /// Idempotent. Quits and joins the reader thread; no Qt callback runs after it returns. Unbounded:
  /// it waits for a blocking driver call to return.
  void stop();

  std::queue<RawCanFrame> drainFrames();
  /// Set once per session: on a ConnectionError, on 10 consecutive ReadErrors with no data frame in
  /// between, when the device leaves ConnectedState, or (socketcan, Linux) when the interface goes
  /// down or disappears. Pushed after every frame received before it, on the same thread.
  std::optional<std::string> takeFatalError();
  ReaderCounts counts() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace can_stream
