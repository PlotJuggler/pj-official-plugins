#pragma once

#include <cstdint>
#include <optional>

namespace can_stream {

/// Maps a CAN adapter's frame timestamp to absolute wall-clock nanoseconds.
///
/// - 0 (the backend stamps nothing): the host arrival time.
/// - At or after 2001-09-09 (e.g. SocketCAN kernel stamps): already absolute, passed through.
/// - Anything else is a relative clock (PEAK/Vector count from driver start): anchored to the host
///   time of the first frame, then spaced by the device clock. A backwards jump of more than one
///   second (replug, driver restart) re-anchors instead of dropping data into the past.
class TimestampAnchor {
 public:
  static constexpr std::int64_t kAbsoluteThresholdNs = 1'000'000'000'000'000'000;  // 2001-09-09
  static constexpr std::int64_t kReanchorNs = 1'000'000'000;                       // 1 s backwards jump

  std::int64_t map(std::int64_t device_ns, std::int64_t host_now_ns) {
    if (device_ns == 0) {
      return host_now_ns;
    }
    if (device_ns >= kAbsoluteThresholdNs) {
      return device_ns;
    }
    if (!offset_ || device_ns + kReanchorNs < last_device_ns_) {
      offset_ = host_now_ns - device_ns;
    }
    last_device_ns_ = device_ns;
    return device_ns + *offset_;
  }

 private:
  std::optional<std::int64_t> offset_;
  std::int64_t last_device_ns_ = 0;
};

}  // namespace can_stream
