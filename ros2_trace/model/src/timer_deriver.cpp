#include "ros2_trace_model/timer_deriver.hpp"

#include <string>

namespace ros2_trace_model {

TimerDeriver::TimerDeriver(const Registry& registry, MetricSampleSink& sink) : registry_(registry), sink_(sink) {}

void TimerDeriver::consume(const RawEvent& ev) {
  if (ev.tp() != Tp::CallbackStart) {
    return;
  }
  const auto cb = ev.handle("callback");
  if (!cb) {
    return;
  }
  const EntityKey key{*cb};
  const auto period = registry_.timerPeriodForCallback(key);
  if (!period) {
    return;  // not a timer callback
  }

  if (const auto prev = last_start_.find(key); prev != last_start_.end()) {
    const std::int64_t interval = ev.ts_ns() - prev->second;
    const double jitter_ms = static_cast<double>(interval - *period) / 1.0e6;

    const auto rc = registry_.resolveCallback(key);
    const std::string node = (rc && !rc->node_name.empty()) ? rc->node_name : "unknown";
    const std::string id = (rc && !rc->symbol.empty()) ? rc->symbol : "timer";
    sink_.onSample(Sample{"/ros2_trace/" + node + "/timers/" + id + "/jitter_ms", ev.ts_ns(), jitter_ms});
  }
  last_start_[key] = ev.ts_ns();
}

}  // namespace ros2_trace_model
