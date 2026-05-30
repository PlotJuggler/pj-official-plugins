#include "ros2_trace_model/latency_deriver.hpp"

namespace ros2_trace_model {

LatencyDeriver::LatencyDeriver(const Registry& registry, MetricSampleSink& sink) : registry_(registry), sink_(sink) {}

void LatencyDeriver::consume(const RawEvent& ev) {
  if (ev.tp() != Tp::RmwTake) {
    return;
  }
  if (ev.boolean("taken") == false) {
    return;  // no message was actually taken
  }
  const auto source_ts = ev.i64("source_timestamp");
  const auto rmw_handle = ev.handle("rmw_subscription_handle");
  if (!source_ts || *source_ts <= 0 || !rmw_handle) {
    return;
  }
  const auto topic = registry_.topicForRmwSubscription(EntityKey{*rmw_handle});
  if (!topic) {
    return;  // subscription init not seen (e.g. late live attach)
  }
  const double latency_ms = static_cast<double>(ev.ts_ns() - *source_ts) / 1.0e6;
  sink_.onSample(Sample{"/ros2_trace" + *topic + "/latency_ms", ev.ts_ns(), latency_ms});
}

}  // namespace ros2_trace_model
