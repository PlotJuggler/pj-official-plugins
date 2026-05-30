#pragma once

#include <cstdint>
#include <unordered_map>

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/sinks.hpp"

namespace ros2_trace_model {

// Tracks per-CPU executor state (waiting / scheduling / executing) and the wait
// duration of each executor cycle (wait_for_work -> execute). Attribution is
// per-CPU because executor tracepoints carry no node/thread in their payload;
// cpu_id comes from the CTF packet context. Events without a cpu fall back to a
// single un-split executor series. Needs no Registry.
class ExecutorDeriver {
 public:
  explicit ExecutorDeriver(MetricSampleSink& sink);

  void consume(const RawEvent& ev);

 private:
  MetricSampleSink& sink_;
  std::unordered_map<std::uint32_t, std::int64_t> wait_start_;  // cpu -> wait_for_work ts
};

}  // namespace ros2_trace_model
