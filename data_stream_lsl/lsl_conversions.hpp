#pragma once

#include <string>
#include <string_view>

namespace pj_lsl {

/// How LSL sample stamps become PlotJuggler absolute epoch-nanoseconds.
enum class TimestampMode {
  kSync,      ///< time_correction() + local_clock->epoch offset (default)
  kRaw,       ///< raw LSL stamp * 1e9 (parity with the PJ3 plugin)
  kReceiver,  ///< system clock at drain time
};

inline TimestampMode parseTimestampMode(std::string_view s) {
  if (s == "raw") {
    return TimestampMode::kRaw;
  }
  if (s == "receiver") {
    return TimestampMode::kReceiver;
  }
  return TimestampMode::kSync;  // default, including unknown values
}

inline const char* toString(TimestampMode mode) {
  switch (mode) {
    case TimestampMode::kRaw:
      return "raw";
    case TimestampMode::kReceiver:
      return "receiver";
    case TimestampMode::kSync:
    default:
      return "sync";
  }
}

}  // namespace pj_lsl
