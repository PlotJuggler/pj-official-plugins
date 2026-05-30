#include "ros2_trace_model/timer_deriver.hpp"

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

void registerTimer(Registry& reg, std::uint64_t node, std::uint64_t timer, std::uint64_t cb, std::int64_t period_ns) {
  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("talker")}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(Tp::RclTimerInit, 2, {{"timer_handle", timer}, {"period", period_ns}}));
  reg.consume(RawEvent(Tp::RclcppTimerLinkNode, 3, {{"timer_handle", timer}, {"node_handle", node}}));
  reg.consume(RawEvent(Tp::RclcppTimerCallbackAdded, 4, {{"timer_handle", timer}, {"callback", cb}}));
  reg.consume(RawEvent(Tp::RclcppCallbackRegister, 5, {{"callback", cb}, {"symbol", std::string("on_timer")}}));
}

}  // namespace

TEST(TimerDeriver, EmitsJitterFromConsecutiveTimerCallbacks) {
  Registry reg;
  const std::uint64_t cb = 0x6000;
  registerTimer(reg, 0x1000, 0x5000, cb, 100'000'000);  // 100 ms nominal period

  CollectingSink sink;
  TimerDeriver deriver(reg, sink);

  deriver.consume(RawEvent(Tp::CallbackStart, 1'000'000'000, {{"callback", cb}}));  // first: no sample
  deriver.consume(RawEvent(Tp::CallbackStart, 1'105'000'000, {{"callback", cb}}));  // interval 105 ms

  ASSERT_EQ(sink.samples.size(), 1u);
  EXPECT_EQ(sink.samples[0].series, "/ros2_trace/talker/timers/on_timer/jitter_ms");
  EXPECT_EQ(sink.samples[0].ts_ns, 1'105'000'000);
  EXPECT_DOUBLE_EQ(std::get<double>(sink.samples[0].value), 5.0);  // 105 - 100
}

TEST(TimerDeriver, IgnoresNonTimerCallbacks) {
  Registry reg;  // no timer registered for this callback
  CollectingSink sink;
  TimerDeriver deriver(reg, sink);

  deriver.consume(RawEvent(Tp::CallbackStart, 1'000'000'000, {{"callback", std::uint64_t{0x6000}}}));
  deriver.consume(RawEvent(Tp::CallbackStart, 1'100'000'000, {{"callback", std::uint64_t{0x6000}}}));

  EXPECT_TRUE(sink.samples.empty());
}
