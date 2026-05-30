#pragma once

#include <optional>

#include "ros2_trace_model/raw_event.hpp"

namespace ros2_trace_model {

// Abstract source of decoded trace events, yielded in timestamp order.
// Implementations: FsTraceSource (babeltrace2 source.ctf.fs), LiveTraceSource
// (lttng-live), and InMemoryTraceSource (tests).
class TraceSource {
 public:
  TraceSource() = default;
  TraceSource(const TraceSource&) = delete;
  TraceSource& operator=(const TraceSource&) = delete;
  virtual ~TraceSource() = default;

  // The next event, or nullopt at end of stream.
  virtual std::optional<RawEvent> next() = 0;
};

}  // namespace ros2_trace_model
