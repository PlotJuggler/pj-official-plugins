#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "ros2_trace_model/entity_key.hpp"

namespace ros2_trace_model {

// A sample value: numeric for metrics (durations, latencies, jitter, the 0/1
// "active" step) or a string for categorical state series (executor, lifecycle).
// Maps directly onto PlotJuggler's ValueRef at the plugin boundary.
using SampleValue = std::variant<double, std::string>;

// A single point on a named PlotJuggler series.
struct Sample {
  std::string series;
  std::int64_t ts_ns{};
  SampleValue value{0.0};
};

// Phase-1 output: derived metric samples that become PlotJuggler y(t) curves.
struct MetricSampleSink {
  MetricSampleSink() = default;
  MetricSampleSink(const MetricSampleSink&) = delete;
  MetricSampleSink& operator=(const MetricSampleSink&) = delete;
  virtual ~MetricSampleSink() = default;

  virtual void onSample(const Sample& sample) = 0;
};

// What an interval/state corresponds to. Phase 1 fakes these as step/string
// signals; Phase 2 renders them as real interval lanes. The sink is defined now
// so the seam exists, even though Phase 1 wires only MetricSampleSink.
enum class EventKind {
  CallbackInterval,
  ExecutorState,
  LifecycleState,
};

struct ResolvedEvent {
  EventKind kind{};
  std::string entity;  // series-path base of the owning entity
  std::int64_t t0{};
  std::optional<std::int64_t> t1;  // end (intervals); absent for instantaneous
  std::string label;
};

struct ResolvedEventSink {
  ResolvedEventSink() = default;
  ResolvedEventSink(const ResolvedEventSink&) = delete;
  ResolvedEventSink& operator=(const ResolvedEventSink&) = delete;
  virtual ~ResolvedEventSink() = default;

  virtual void onEvent(const ResolvedEvent& event) = 0;
};

}  // namespace ros2_trace_model
