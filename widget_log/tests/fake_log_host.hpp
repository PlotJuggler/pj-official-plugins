// A widget host holding object samples per wire, served through the real PJ::WidgetHostBridge so the
// plugin sees the C ABI. History follows the host contract: inclusive display-time range, ascending
// time, `max_samples == 0` = a default cap, an explicit cap honoured (up to `hard_cap`, which an older
// host had), even decimation keeping the first and last sample, out_total = samples in range.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <pj_base/builtin/log_codec.hpp>
#include <pj_plugins/host/widget_host_bridge.hpp>
#include <string>
#include <utility>
#include <vector>

namespace log_viewer_test {

inline PJ::sdk::Log makeLog(
    PJ::sdk::Log::Level level, std::string message, std::string node = "/node", std::string file = {},
    std::string function = {}, uint32_t line = 0, int64_t stamp_ns = 0) {
  PJ::sdk::Log log;
  log.timestamp_ns = stamp_ns;
  log.level = level;
  log.message = std::move(message);
  log.name = std::move(node);
  log.file = std::move(file);
  log.function = std::move(function);
  log.line = line;
  return log;
}

class FakeLogHost {
 public:
  struct Sample {
    double time = 0.0;
    std::vector<uint8_t> bytes;
  };
  struct Wire {
    std::string label;
    std::string type_name = "kLog";
    std::vector<Sample> samples;  ///< ascending time
  };

  std::map<uint32_t, Wire> wires;  ///< by slot
  uint32_t default_cap = 10'000;
  uint32_t hard_cap = std::numeric_limits<uint32_t>::max();
  std::vector<PJ::WidgetHistoryQuery> queries;
  std::vector<std::string> warnings;
  PJ::WidgetHostBridge bridge;

  FakeLogHost() {
    bridge.history = [this](const PJ::WidgetHistoryQuery& query, const PJ::WidgetValueSink& sink) {
      return history(query, sink);
    };
    bridge.report_message = [this](PJ::WidgetMessageLevel level, std::string_view message) {
      if (level == PJ::WidgetMessageLevel::kWarning) {
        warnings.emplace_back(message);
      }
    };
  }
  FakeLogHost(const FakeLogHost&) = delete;
  FakeLogHost& operator=(const FakeLogHost&) = delete;

  Wire& wire(uint32_t slot, std::string label) {
    Wire& entry = wires[slot];
    entry.label = std::move(label);
    return entry;
  }

  /// Insert a sample after any of equal time, as a store keeps arrival order.
  void add(uint32_t slot, double time, const PJ::sdk::Log& log) {
    addBytes(slot, time, PJ::serializeLog(log));
  }
  void addBytes(uint32_t slot, double time, std::vector<uint8_t> bytes) {
    auto& samples = wires.at(slot).samples;
    const auto at = std::upper_bound(
        samples.begin(), samples.end(), time, [](double t, const Sample& sample) { return t < sample.time; });
    samples.insert(at, Sample{time, std::move(bytes)});
  }

  /// What on_update carries at @p tracker: per wire, the latest sample at or before it.
  std::vector<PJ_widget_value_t> values(double tracker) const {
    std::vector<PJ_widget_value_t> out;
    for (const auto& [slot, entry] : wires) {
      PJ_widget_value_t value{};
      value.input = 0;
      value.slot = slot;
      value.label = PJ_string_view_t{entry.label.data(), entry.label.size()};
      const auto after = std::upper_bound(
          entry.samples.begin(), entry.samples.end(), tracker,
          [](double t, const Sample& sample) { return t < sample.time; });
      if (after != entry.samples.begin()) {
        const Sample& sample = *(after - 1);
        value.kind = PJ_WIDGET_VALUE_OBJECT;
        value.time = sample.time;
        value.bytes = PJ_bytes_view_t{sample.bytes.data(), sample.bytes.size()};
        value.type_name = PJ_string_view_t{entry.type_name.data(), entry.type_name.size()};
      }
      out.push_back(value);
    }
    return out;
  }

  PJ::Expected<uint32_t> history(const PJ::WidgetHistoryQuery& query, const PJ::WidgetValueSink& sink) {
    queries.push_back(query);
    const auto found = wires.find(query.slot);
    if (query.input != 0 || found == wires.end()) {
      return PJ::unexpected(std::string("not wired"));
    }
    const Wire& entry = found->second;
    std::vector<size_t> in_range;
    for (size_t i = 0; i < entry.samples.size(); ++i) {
      if (entry.samples[i].time >= query.from && entry.samples[i].time <= query.to) {
        in_range.push_back(i);
      }
    }
    const size_t total = in_range.size();
    const size_t cap =
        std::max<size_t>(1, query.max_samples == 0 ? default_cap : std::min(query.max_samples, hard_cap));
    std::vector<size_t> picked;
    if (total <= cap) {
      picked = in_range;
    } else {
      for (size_t j = 0; j < cap; ++j) {
        const size_t denominator = std::max<size_t>(cap - 1, 1);
        picked.push_back(in_range[j * (total - 1) / denominator]);
      }
    }
    for (size_t index : picked) {
      const Sample& sample = entry.samples[index];
      PJ_widget_value_t value{};
      value.input = query.input;
      value.slot = query.slot;
      value.kind = PJ_WIDGET_VALUE_OBJECT;
      value.time = sample.time;
      value.bytes = PJ_bytes_view_t{sample.bytes.data(), sample.bytes.size()};
      value.type_name = PJ_string_view_t{entry.type_name.data(), entry.type_name.size()};
      value.label = PJ_string_view_t{entry.label.data(), entry.label.size()};
      if (!sink(value)) {
        break;
      }
    }
    return static_cast<uint32_t>(total);
  }
};

}  // namespace log_viewer_test
