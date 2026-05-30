#pragma once

#include "ros2_trace_model/callback_deriver.hpp"
#include "ros2_trace_model/latency_deriver.hpp"
#include "ros2_trace_model/lifecycle_deriver.hpp"
#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"
#include "ros2_trace_model/timer_deriver.hpp"
#include "ros2_trace_model/trace_source.hpp"

namespace ros2_trace_model {

// Drives a TraceSource through the Registry and the derivers into the sinks.
// Every event updates the Registry first (so one-time init data is recorded
// before any runtime event needs it), then is dispatched to each deriver. The
// same per-event path serves the file (run-to-completion) and live use cases.
class Pipeline {
 public:
  explicit Pipeline(MetricSampleSink& metric_sink);

  // Consume the whole source to completion (file path).
  void run(TraceSource& source);

  // Feed a single decoded event.
  void consume(const RawEvent& ev);

 private:
  Registry registry_;
  MetricSampleSink& metric_sink_;
  CallbackDeriver callback_deriver_;
  LatencyDeriver latency_deriver_;
  TimerDeriver timer_deriver_;
  LifecycleDeriver lifecycle_deriver_;
};

}  // namespace ros2_trace_model
