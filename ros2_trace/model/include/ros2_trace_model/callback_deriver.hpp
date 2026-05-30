#pragma once

#include <cstdint>
#include <unordered_map>

#include "ros2_trace_model/entity_key.hpp"
#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"

namespace ros2_trace_model {

// Pairs callback_start with callback_end (by callback handle) and emits a
// duration metric per completed callback, resolving the callback to a named
// series via the Registry. Online state machine: works identically for finite
// (file) and unbounded (live) streams.
class CallbackDeriver {
 public:
  CallbackDeriver(const Registry& registry, MetricSampleSink& sink);

  void consume(const RawEvent& ev);

 private:
  const Registry& registry_;
  MetricSampleSink& sink_;
  std::unordered_map<EntityKey, std::int64_t, EntityKeyHash> open_starts_;
};

}  // namespace ros2_trace_model
