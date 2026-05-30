#pragma once

#include <cstdint>
#include <unordered_map>

#include "ros2_trace_model/entity_key.hpp"
#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"

namespace ros2_trace_model {

// Emits timer scheduling jitter (ms): the deviation of each inter-callback
// interval from the timer's nominal period. Needs two consecutive callback_start
// events for the same timer, so the first one produces no sample.
class TimerDeriver {
 public:
  TimerDeriver(const Registry& registry, MetricSampleSink& sink);

  void consume(const RawEvent& ev);

 private:
  const Registry& registry_;
  MetricSampleSink& sink_;
  std::unordered_map<EntityKey, std::int64_t, EntityKeyHash> last_start_;
};

}  // namespace ros2_trace_model
