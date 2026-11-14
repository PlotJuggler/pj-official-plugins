// The SDK's fake widget host (PJ::sdk::testing::FakeWidgetHost) with log conveniences: wires of the widget's one input
// carrying kLog objects, and samples added at display times. History semantics (inclusive range, caps, decimation,
// nanosecond store times) are the shared fake's.
#pragma once

#include <chrono>
#include <cstdint>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/builtin/log_codec.hpp>
#include <pj_plugins/sdk/testing/fake_widget_host.hpp>
#include <string>
#include <utility>
#include <vector>

namespace log_viewer_test {

inline PJ::sdk::Log makeLog(
    PJ::sdk::Log::Level level, std::string message, std::string source = "/node", std::string category = {},
    int64_t stamp_ns = 0) {
  PJ::sdk::Log log;
  log.timestamp_ns = stamp_ns;
  log.level = level;
  log.message = std::move(message);
  log.source = std::move(source);
  log.category = std::move(category);
  return log;
}

class FakeLogHost : public PJ::sdk::testing::FakeWidgetHost {
 public:
  /// The widget's one input, "logs".
  static constexpr uint32_t kInput = 0;

  using FakeWidgetHost::wire;

  /// A wire of the "logs" input; a new one carries kLog objects.
  Wire& wire(uint32_t slot, std::string label) {
    Wire& entry = wire(kInput, slot, std::move(label));
    if (entry.type_name.empty()) {
      entry.type_name = std::string(PJ::sdk::name(PJ::sdk::BuiltinObjectType::kLog));
    }
    return entry;
  }

  void add(uint32_t slot, double time, const PJ::sdk::Log& log) {
    addBytes(slot, time, PJ::serializeLog(log));
  }
  void addBytes(uint32_t slot, double time, std::vector<uint8_t> bytes) {
    insert(kInput, slot, Sample{.store_time = storeTime(time), .bytes = std::move(bytes)});
  }
  /// A log at @p store_time, for samples at epoch nanoseconds.
  void addAtStoreTime(uint32_t slot, std::chrono::nanoseconds store_time, const PJ::sdk::Log& log) {
    insert(kInput, slot, Sample{.store_time = store_time, .bytes = PJ::serializeLog(log)});
  }

  [[nodiscard]] std::vector<Sample>& samples(uint32_t slot) {
    return wires.at({kInput, slot}).samples;
  }
  [[nodiscard]] std::vector<std::string> warnings() const {
    return messagesAt(PJ::WidgetMessageLevel::kWarning);
  }
};

}  // namespace log_viewer_test
