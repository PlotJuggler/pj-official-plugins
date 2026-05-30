#include "ros2_trace_model/executor_deriver.hpp"

#include <optional>
#include <string>

namespace ros2_trace_model {

namespace {

// No-cpu sentinel for the wait_start_ map key when an event lacks packet cpu.
constexpr std::uint32_t kNoCpu = 0xFFFFFFFFu;

std::string executorBase(std::optional<std::uint32_t> cpu) {
  if (cpu) {
    return "/ros2_trace/executor/cpu" + std::to_string(*cpu);
  }
  return "/ros2_trace/executor";
}

}  // namespace

ExecutorDeriver::ExecutorDeriver(MetricSampleSink& sink) : sink_(sink) {}

void ExecutorDeriver::consume(const RawEvent& ev) {
  const std::string base = executorBase(ev.cpu());
  const std::uint32_t key = ev.cpu().value_or(kNoCpu);

  switch (ev.tp()) {
    case Tp::RclcppExecutorWaitForWork:
      sink_.onSample(Sample{base + "/state", ev.ts_ns(), std::string("waiting")});
      wait_start_[key] = ev.ts_ns();
      break;
    case Tp::RclcppExecutorGetNextReady:
      sink_.onSample(Sample{base + "/state", ev.ts_ns(), std::string("scheduling")});
      break;
    case Tp::RclcppExecutorExecute: {
      sink_.onSample(Sample{base + "/state", ev.ts_ns(), std::string("executing")});
      // First execute after a wait closes the wait cycle: emit its duration once.
      if (const auto it = wait_start_.find(key); it != wait_start_.end()) {
        const double wait_ms = static_cast<double>(ev.ts_ns() - it->second) / 1.0e6;
        sink_.onSample(Sample{base + "/wait_ms", ev.ts_ns(), wait_ms});
        wait_start_.erase(it);
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace ros2_trace_model
