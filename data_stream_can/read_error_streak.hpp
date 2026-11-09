#pragma once

namespace can_stream {

/// Decides when QCanBusDevice::ReadError means the adapter is gone. One ReadError is not: socketcan
/// raises it for a single incomplete or invalid frame and keeps reading, PEAK for any non-empty-queue
/// status (an overrun, too). A PEAK unplug repeats it on every read tick, with no frame in between.
///
/// ponytail: a fixed count of consecutive errors. An unplug is detected ~10 read ticks late, and a
/// pathological stream of bad frames with no good one in between could still end a healthy session;
/// switch to a time window (no data frame for N s while erroring) if either shows up.
class ReadErrorStreak {
 public:
  static constexpr int kFatalReadErrorStreak = 10;

  /// True once kFatalReadErrorStreak ReadErrors arrived with no data frame in between.
  bool onReadError() {
    if (streak_ < kFatalReadErrorStreak) {  // saturate: an endless error stream must not overflow
      ++streak_;
    }
    return streak_ >= kFatalReadErrorStreak;
  }
  void onFrame() {
    streak_ = 0;
  }

 private:
  int streak_ = 0;
};

}  // namespace can_stream
