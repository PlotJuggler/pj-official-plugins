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

TEST(Registry, ResolvesTimerCallbackChain) {
  Registry reg;
  const std::uint64_t node = 0x1000;
  const std::uint64_t timer = 0x5000;
  const std::uint64_t cb = 0x6000;

  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("talker")}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(Tp::RclTimerInit, 2, {{"timer_handle", timer}, {"period", std::int64_t{100000000}}}));
  reg.consume(RawEvent(Tp::RclcppTimerLinkNode, 3, {{"timer_handle", timer}, {"node_handle", node}}));
  reg.consume(RawEvent(Tp::RclcppTimerCallbackAdded, 4, {{"timer_handle", timer}, {"callback", cb}}));
  reg.consume(RawEvent(Tp::RclcppCallbackRegister, 5, {{"callback", cb}, {"symbol", std::string("on_timer")}}));

  const auto rc = reg.resolveCallback(EntityKey{cb});
  ASSERT_TRUE(rc.has_value());
  EXPECT_EQ(rc->kind, CallbackKind::Timer);
  EXPECT_EQ(rc->node_name, "talker");
  EXPECT_EQ(rc->symbol, "on_timer");
  EXPECT_TRUE(rc->topic_name.empty());
}

TEST(Registry, ResolvesServiceCallbackChain) {
  Registry reg;
  const std::uint64_t node = 0x1000;
  const std::uint64_t service = 0x8000;
  const std::uint64_t cb = 0x9000;

  reg.consume(RawEvent(
      Tp::RclNodeInit, 1,
      {{"node_handle", node}, {"node_name", std::string("srv_node")}, {"namespace", std::string("/")}}));
  reg.consume(RawEvent(
      Tp::RclServiceInit, 2,
      {{"service_handle", service}, {"node_handle", node}, {"service_name", std::string("/add_two_ints")}}));
  reg.consume(RawEvent(Tp::RclcppServiceCallbackAdded, 3, {{"service_handle", service}, {"callback", cb}}));
  reg.consume(RawEvent(Tp::RclcppCallbackRegister, 4, {{"callback", cb}, {"symbol", std::string("on_request")}}));

  const auto rc = reg.resolveCallback(EntityKey{cb});
  ASSERT_TRUE(rc.has_value());
  EXPECT_EQ(rc->kind, CallbackKind::Service);
  EXPECT_EQ(rc->node_name, "srv_node");
  EXPECT_EQ(rc->topic_name, "/add_two_ints");
  EXPECT_EQ(rc->symbol, "on_request");
}
