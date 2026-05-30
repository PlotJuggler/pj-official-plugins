#include "ros2_trace_model/callback_deriver.hpp"

#include <string>

namespace ros2_trace_model {

namespace {

// Series-path base identifying a callback, e.g.
// "/ros2_trace/<node>/callbacks/<symbol>". Falls back to placeholders when the
// callback could not be resolved (missing init events).
std::string callbackSeriesBase(const Registry& reg, EntityKey callback) {
  std::string node = "unknown";
  std::string leaf = "callback";
  if (const auto rc = reg.resolveCallback(callback)) {
    if (!rc->node_name.empty()) {
      node = rc->node_name;
    }
    if (!rc->symbol.empty()) {
      leaf = rc->symbol;
    }
  }
  return "/ros2_trace/" + node + "/callbacks/" + leaf;
}

}  // namespace

CallbackDeriver::CallbackDeriver(const Registry& registry, MetricSampleSink& sink) : registry_(registry), sink_(sink) {}

void CallbackDeriver::consume(const RawEvent& ev) {
  switch (ev.tp()) {
    case Tp::CallbackStart: {
      if (const auto cb = ev.handle("callback")) {
        const EntityKey key{*cb};
        open_starts_[key] = ev.ts_ns();
        // Interval "active" step: rises to 1 while the callback runs.
        sink_.onSample(Sample{callbackSeriesBase(registry_, key) + "/active", ev.ts_ns(), 1.0});
      }
      break;
    }
    case Tp::CallbackEnd: {
      const auto cb = ev.handle("callback");
      if (!cb) {
        break;
      }
      const EntityKey key{*cb};
      const auto it = open_starts_.find(key);
      if (it == open_starts_.end()) {
        break;  // end without a matching start (e.g. started before trace began)
      }
      const std::int64_t start_ns = it->second;
      open_starts_.erase(it);

      const std::string base = callbackSeriesBase(registry_, key);
      const double duration_ms = static_cast<double>(ev.ts_ns() - start_ns) / 1.0e6;
      sink_.onSample(Sample{base + "/duration_ms", start_ns, duration_ms});
      // Interval "active" step falls back to 0 when the callback returns.
      sink_.onSample(Sample{base + "/active", ev.ts_ns(), 0.0});
      break;
    }
    default:
      break;
  }
}

}  // namespace ros2_trace_model
