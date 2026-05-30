#include "ros2_trace_model/callback_deriver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
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

  ASSERT_EQ(sink.samples.size(), 1u);
  EXPECT_EQ(sink.samples[0].series, "/ros2_trace/listener/callbacks/on_msg/duration_ms");
  EXPECT_EQ(sink.samples[0].ts_ns, 1'000'000);
  EXPECT_DOUBLE_EQ(std::get<double>(sink.samples[0].value), 0.5);  // (1.5ms - 1.0ms)
}
