#include <gtest/gtest.h>

#include "ros2_trace_model/raw_event.hpp"

using namespace ros2_trace_model;

TEST(ClassifyTracepoint, MapsKnownNamesWithProviderPrefix) {
  EXPECT_EQ(classifyTracepoint("ros2:callback_start"), Tp::CallbackStart);
  EXPECT_EQ(classifyTracepoint("ros2:callback_end"), Tp::CallbackEnd);
  EXPECT_EQ(classifyTracepoint("ros2:rmw_take"), Tp::RmwTake);
  EXPECT_EQ(classifyTracepoint("ros2:rmw_publish"), Tp::RmwPublish);
  EXPECT_EQ(classifyTracepoint("ros2:rcl_node_init"), Tp::RclNodeInit);
  EXPECT_EQ(classifyTracepoint("ros2:rcl_subscription_init"), Tp::RclSubscriptionInit);
  EXPECT_EQ(classifyTracepoint("ros2:rclcpp_subscription_callback_added"), Tp::RclcppSubscriptionCallbackAdded);
  EXPECT_EQ(classifyTracepoint("ros2:rcl_timer_init"), Tp::RclTimerInit);
  EXPECT_EQ(classifyTracepoint("ros2:rclcpp_callback_register"), Tp::RclcppCallbackRegister);
  EXPECT_EQ(classifyTracepoint("ros2:rcl_lifecycle_transition"), Tp::RclLifecycleTransition);
  EXPECT_EQ(classifyTracepoint("ros2:rclcpp_executor_execute"), Tp::RclcppExecutorExecute);
}

TEST(ClassifyTracepoint, WorksWithoutProviderPrefix) {
  EXPECT_EQ(classifyTracepoint("callback_start"), Tp::CallbackStart);
  EXPECT_EQ(classifyTracepoint("rcl_node_init"), Tp::RclNodeInit);
}

TEST(ClassifyTracepoint, UnknownIsOther) {
  EXPECT_EQ(classifyTracepoint("ros2:some_future_tracepoint"), Tp::Other);
  EXPECT_EQ(classifyTracepoint("sched_switch"), Tp::Other);
  EXPECT_EQ(classifyTracepoint(""), Tp::Other);
}
