#include "ros2_trace_model/pipeline.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ros2_trace_model/raw_event.hpp"
#include "ros2_trace_model/sinks.hpp"
#include "ros2_trace_model/trace_source.hpp"

using namespace ros2_trace_model;

namespace {

struct CollectingSink : MetricSampleSink {
  std::vector<Sample> samples;
  void onSample(const Sample& s) override {
    samples.push_back(s);
  }
};

struct InMemoryTraceSource : TraceSource {
  std::vector<RawEvent> events;
  std::size_t index = 0;
  std::optional<RawEvent> next() override {
    if (index >= events.size()) {
      return std::nullopt;
    }
    return events[index++];
  }
};

}  // namespace

TEST(Pipeline, ResolvesAndEmitsCallbackDurationEndToEnd) {
  const std::uint64_t node = 0x1000;
  const std::uint64_t sub_handle = 0x2000;
  const std::uint64_t sub_obj = 0x3000;
  const std::uint64_t cb = 0x4000;

  InMemoryTraceSource src;
  src.events.push_back(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("listener")}, {"namespace", std::string("/")}}));
  src.events.push_back(RawEvent(
      Tp::RclSubscriptionInit, 2,
      {{"subscription_handle", sub_handle}, {"node_handle", node}, {"topic_name", std::string("/chatter")}}));
  src.events.push_back(
      RawEvent(Tp::RclcppSubscriptionInit, 3, {{"subscription_handle", sub_handle}, {"subscription", sub_obj}}));
  src.events.push_back(RawEvent(Tp::RclcppSubscriptionCallbackAdded, 4, {{"subscription", sub_obj}, {"callback", cb}}));
  src.events.push_back(RawEvent(Tp::RclcppCallbackRegister, 5, {{"callback", cb}, {"symbol", std::string("on_msg")}}));
  src.events.push_back(RawEvent(Tp::CallbackStart, 1'000'000, {{"callback", cb}, {"is_intra_process", false}}));
  src.events.push_back(RawEvent(Tp::CallbackEnd, 3'000'000, {{"callback", cb}}));

  CollectingSink sink;
  Pipeline pipeline(sink);
  pipeline.run(src);

  ASSERT_EQ(sink.samples.size(), 1u);
  EXPECT_EQ(sink.samples[0].series, "/ros2_trace/listener/callbacks/on_msg/duration_ms");
  EXPECT_EQ(sink.samples[0].ts_ns, 1'000'000);
  EXPECT_DOUBLE_EQ(sink.samples[0].value, 2.0);  // (3.0ms - 1.0ms)
}
