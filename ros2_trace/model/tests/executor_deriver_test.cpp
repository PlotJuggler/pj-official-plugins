#include "ros2_trace_model/executor_deriver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ros2_trace_model/raw_event.hpp"
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

TEST(ExecutorDeriver, EmitsPerCpuStateAndWaitDuration) {
  CollectingSink sink;
  ExecutorDeriver deriver(sink);
  const std::optional<std::uint32_t> cpu = 2;

  deriver.consume(RawEvent(Tp::RclcppExecutorWaitForWork, 1'000'000, {{"timeout", std::int64_t{-1}}}, cpu));
  deriver.consume(RawEvent(Tp::RclcppExecutorGetNextReady, 1'200'000, {}, cpu));
  deriver.consume(RawEvent(Tp::RclcppExecutorExecute, 1'500'000, {{"handle", std::uint64_t{0xABC}}}, cpu));

  bool found_wait = false;
  bool found_executing_state = false;
  for (const auto& s : sink.samples) {
    if (s.series == "/ros2_trace/executor/cpu2/wait_ms") {
      found_wait = true;
      EXPECT_EQ(s.ts_ns, 1'500'000);
      EXPECT_DOUBLE_EQ(std::get<double>(s.value), 0.5);  // 1.5ms - 1.0ms
    }
    if (s.series == "/ros2_trace/executor/cpu2/state" && std::holds_alternative<std::string>(s.value) &&
        std::get<std::string>(s.value) == "executing") {
      found_executing_state = true;
    }
  }
  EXPECT_TRUE(found_wait);
  EXPECT_TRUE(found_executing_state);
}
