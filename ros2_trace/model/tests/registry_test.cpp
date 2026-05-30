#include "ros2_trace_model/registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "ros2_trace_model/raw_event.hpp"

using namespace ros2_trace_model;

TEST(Registry, RecordsNodeNameFromNodeInit) {
  Registry reg;
  const std::uint64_t node_handle = 0x1000;

  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node_handle}, {"node_name", std::string("listener")}, {"namespace", std::string("/demo")}}));

  const auto info = reg.node(EntityKey{node_handle});
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->name, "listener");
  EXPECT_EQ(info->ns, "/demo");
}

TEST(Registry, ResolvesSubscriptionCallbackChain) {
  Registry reg;
  const std::uint64_t node = 0x1000;
  const std::uint64_t sub_handle = 0x2000;
  const std::uint64_t rmw_sub = 0x2500;
  const std::uint64_t sub_obj = 0x3000;
  const std::uint64_t cb = 0x4000;

  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("listener")}, {"namespace", std::string("/demo")}}));
  reg.consume(RawEvent(
      Tp::RclSubscriptionInit, 2,
      {{"subscription_handle", sub_handle},
       {"node_handle", node},
       {"rmw_subscription_handle", rmw_sub},
       {"topic_name", std::string("/chatter")}}));
  reg.consume(
      RawEvent(Tp::RclcppSubscriptionInit, 3, {{"subscription_handle", sub_handle}, {"subscription", sub_obj}}));
  reg.consume(RawEvent(Tp::RclcppSubscriptionCallbackAdded, 4, {{"subscription", sub_obj}, {"callback", cb}}));
  reg.consume(RawEvent(
      Tp::RclcppCallbackRegister, 5,
      {{"callback", cb}, {"symbol", std::string("Listener::on_msg(std_msgs::msg::String)")}}));

  const auto rc = reg.resolveCallback(EntityKey{cb});
  ASSERT_TRUE(rc.has_value());
  EXPECT_EQ(rc->kind, CallbackKind::Subscription);
  EXPECT_EQ(rc->node_name, "listener");
  EXPECT_EQ(rc->topic_name, "/chatter");
  EXPECT_EQ(rc->symbol, "Listener::on_msg(std_msgs::msg::String)");
}
