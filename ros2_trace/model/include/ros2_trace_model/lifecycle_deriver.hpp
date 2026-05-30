#pragma once

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"

namespace ros2_trace_model {

// Emits the lifecycle state of managed nodes as a categorical string series,
// one sample per rcl_lifecycle_transition. By convention the state persists
// until the next sample (the PlotJuggler string-series rendering).
class LifecycleDeriver {
 public:
  LifecycleDeriver(const Registry& registry, MetricSampleSink& sink);

  void consume(const RawEvent& ev);

 private:
  const Registry& registry_;
  MetricSampleSink& sink_;
};

}  // namespace ros2_trace_model
