#include "ros2_trace_model/callback_deriver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/registry.hpp"
#include "ros2_trace_model/sinks.hpp"

using namespace ros2_trace_model;

namespace {

struct CollectingSink : MetricSampleSink {
  std::vector<Sample> samples;
  void onSample(const Sample& s) override {
    samples.push_back(s);
  }
};

// A subscription callback wired so the registry can resolve it.
void registerSubscriptionCallback(
    Registry& reg, std::uint64_t node, std::uint64_t sub_handle, std::uint64_t sub_obj, std::uint64_t cb,
    const std::string& node_name, const std::string& topic, const std::string& symbol) {
  reg.consume(
      RawEvent(Tp::RclNodeInit, 1, {{"node_handle", node}, {"node_name", node_name}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(
      Tp::RclSubscriptionInit, 2, {{"subscription_handle", sub_handle}, {"node_handle", node}, {"topic_name", topic}}));
  reg.consume(
      RawEvent(Tp::RclcppSubscriptionInit, 3, {{"subscription_handle", sub_handle}, {"subscription", sub_obj}}));
  reg.consume(RawEvent(Tp::RclcppSubscriptionCallbackAdded, 4, {{"subscription", sub_obj}, {"callback", cb}}));
  reg.consume(RawEvent(Tp::RclcppCallbackRegister, 5, {{"callback", cb}, {"symbol", symbol}}));
}

}  // namespace

TEST(CallbackDeriver, EmitsDurationMillisOnCallbackEnd) {
  Registry reg;
  const std::uint64_t cb = 0x4000;
  registerSubscriptionCallback(reg, 0x1000, 0x2000, 0x3000, cb, "listener", "/chatter", "on_msg");

  CollectingSink sink;
  CallbackDeriver deriver(reg, sink);

  deriver.consume(RawEvent(Tp::CallbackStart, 1'000'000, {{"callback", cb}, {"is_intra_process", false}}));
  deriver.consume(RawEvent(Tp::CallbackEnd, 1'500'000, {{"callback", cb}}));

  const Sample* duration = nullptr;
  for (const auto& s : sink.samples) {
    if (s.series == "/ros2_trace/listener/callbacks/on_msg/duration_ms") {
      duration = &s;
    }
  }
  ASSERT_NE(duration, nullptr);
  EXPECT_EQ(duration->ts_ns, 1'000'000);
  EXPECT_DOUBLE_EQ(std::get<double>(duration->value), 0.5);  // (1.5ms - 1.0ms)
}

TEST(CallbackDeriver, EmitsActiveStepInterval) {
  Registry reg;
  const std::uint64_t cb = 0x4000;
  registerSubscriptionCallback(reg, 0x1000, 0x2000, 0x3000, cb, "listener", "/chatter", "on_msg");

  CollectingSink sink;
  CallbackDeriver deriver(reg, sink);

  deriver.consume(RawEvent(Tp::CallbackStart, 1'000'000, {{"callback", cb}}));
  deriver.consume(RawEvent(Tp::CallbackEnd, 1'500'000, {{"callback", cb}}));

  std::optional<double> active_at_start;
  std::optional<double> active_at_end;
  for (const auto& s : sink.samples) {
    if (s.series == "/ros2_trace/listener/callbacks/on_msg/active") {
      if (s.ts_ns == 1'000'000) {
        active_at_start = std::get<double>(s.value);
      }
      if (s.ts_ns == 1'500'000) {
        active_at_end = std::get<double>(s.value);
      }
    }
  }
  ASSERT_TRUE(active_at_start.has_value());
  ASSERT_TRUE(active_at_end.has_value());
  EXPECT_DOUBLE_EQ(*active_at_start, 1.0);
  EXPECT_DOUBLE_EQ(*active_at_end, 0.0);
}
