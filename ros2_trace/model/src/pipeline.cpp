#include "ros2_trace_model/pipeline.hpp"

namespace ros2_trace_model {

Pipeline::Pipeline(MetricSampleSink& metric_sink)
    : metric_sink_(metric_sink),
      callback_deriver_(registry_, metric_sink_),
      latency_deriver_(registry_, metric_sink_),
      timer_deriver_(registry_, metric_sink_),
      lifecycle_deriver_(registry_, metric_sink_) {}

void Pipeline::run(TraceSource& source) {
  while (std::optional<RawEvent> ev = source.next()) {
    consume(*ev);
  }
}

void Pipeline::consume(const RawEvent& ev) {
  // Init data must be recorded before any runtime event resolves against it;
  // events arrive in timestamp order, so updating the Registry first suffices.
  registry_.consume(ev);
  callback_deriver_.consume(ev);
  latency_deriver_.consume(ev);
  timer_deriver_.consume(ev);
  lifecycle_deriver_.consume(ev);
}

}  // namespace ros2_trace_model
