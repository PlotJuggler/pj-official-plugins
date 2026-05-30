#pragma once

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"

namespace ros2_trace_model {

// Emits per-message transport latency at the subscriber: the gap between a
// message's DDS source timestamp (carried in rmw_take) and the time it was
// taken. Single-event derivation — no cross-process matching needed because
// rmw_take already carries source_timestamp.
class LatencyDeriver {
 public:
  LatencyDeriver(const Registry& registry, MetricSampleSink& sink);

  void consume(const RawEvent& ev);

 private:
  const Registry& registry_;
  MetricSampleSink& sink_;
};

}  // namespace ros2_trace_model
