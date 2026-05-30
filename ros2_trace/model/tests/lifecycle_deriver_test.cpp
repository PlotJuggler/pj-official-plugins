#include "ros2_trace_model/lifecycle_deriver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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

}  // namespace

TEST(LifecycleDeriver, EmitsGoalStateAsStringSeries) {
  Registry reg;
  const std::uint64_t node = 0x1000;
  const std::uint64_t sm = 0x7000;
  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("lc_node")}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(Tp::RclLifecycleStateMachineInit, 2, {{"node_handle", node}, {"state_machine", sm}}));

  CollectingSink sink;
  LifecycleDeriver deriver(reg, sink);
  deriver.consume(RawEvent(
      Tp::RclLifecycleTransition, 5'000'000,
      {{"state_machine", sm}, {"start_label", std::string("unconfigured")}, {"goal_label", std::string("inactive")}}));

  ASSERT_EQ(sink.samples.size(), 1u);
  EXPECT_EQ(sink.samples[0].series, "/ros2_trace/lc_node/lifecycle/state");
  EXPECT_EQ(sink.samples[0].ts_ns, 5'000'000);
  EXPECT_EQ(std::get<std::string>(sink.samples[0].value), "inactive");
}
