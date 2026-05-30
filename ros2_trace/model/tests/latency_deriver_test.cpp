#include "ros2_trace_model/latency_deriver.hpp"

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

}  // namespace

TEST(LatencyDeriver, EmitsMessageLatencyOnTake) {
  Registry reg;
  const std::uint64_t node = 0x1000;
  const std::uint64_t sub_handle = 0x2000;
  const std::uint64_t rmw_sub = 0x2500;
  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("listener")}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(
      Tp::RclSubscriptionInit, 2,
      {{"subscription_handle", sub_handle},
       {"node_handle", node},
       {"rmw_subscription_handle", rmw_sub},
       {"topic_name", std::string("/chatter")}}));

  CollectingSink sink;
  LatencyDeriver deriver(reg, sink);

  // Message published at t=1ms (DDS source_timestamp), taken at t=3ms -> 2ms latency.
  deriver.consume(RawEvent(
      Tp::RmwTake, 3'000'000,
      {{"rmw_subscription_handle", rmw_sub}, {"source_timestamp", std::int64_t{1'000'000}}, {"taken", true}}));

  ASSERT_EQ(sink.samples.size(), 1u);
  EXPECT_EQ(sink.samples[0].series, "/ros2_trace/chatter/latency_ms");
  EXPECT_EQ(sink.samples[0].ts_ns, 3'000'000);
  EXPECT_DOUBLE_EQ(sink.samples[0].value, 2.0);
}

TEST(LatencyDeriver, IgnoresFailedTake) {
  Registry reg;
  const std::uint64_t rmw_sub = 0x2500;
  CollectingSink sink;
  LatencyDeriver deriver(reg, sink);

  deriver.consume(RawEvent(
      Tp::RmwTake, 3'000'000,
      {{"rmw_subscription_handle", rmw_sub}, {"source_timestamp", std::int64_t{0}}, {"taken", false}}));

  EXPECT_TRUE(sink.samples.empty());
}
