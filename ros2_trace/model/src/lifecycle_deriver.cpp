#include "ros2_trace_model/lifecycle_deriver.hpp"

namespace ros2_trace_model {

LifecycleDeriver::LifecycleDeriver(const Registry& registry, MetricSampleSink& sink)
    : registry_(registry), sink_(sink) {}

void LifecycleDeriver::consume(const RawEvent& ev) {
  if (ev.tp() != Tp::RclLifecycleTransition) {
    return;
  }
  const auto sm = ev.handle("state_machine");
  const auto goal = ev.str("goal_label");
  if (!sm || !goal) {
    return;
  }
  const std::string node = registry_.nodeForStateMachine(EntityKey{*sm}).value_or("unknown");
  sink_.onSample(Sample{"/ros2_trace/" + node + "/lifecycle/state", ev.ts_ns(), std::string(*goal)});
}

}  // namespace ros2_trace_model
