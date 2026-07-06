/**
 * @file ros2_topic_subscription_test.cpp
 * @brief Unit tests for the demand-driven per-topic subscription control plane
 *  (pj.topic_subscription.v1), factored into ros2_topic_subscription.hpp so it
 *  is testable without a live ROS 2 graph or rclcpp:
 *
 *   - DesiredTopicsSlot: the mutex-protected "latest wins" slot written by
 *     set_active_topics (host GUI thread) and drained by onPoll (poll thread).
 *   - computeRos2SubscriptionDiff: the pure subscribe/unsubscribe diff between
 *     the source's live subscriptions and the host's desired active-topic set.
 *   - passesAdvertiseFilter / filteredDiscoveredTopics: the dialog-selection-
 *     as-advertise-filter predicate and its application to the subscribable set.
 *   - splitTopicTypes: the multi-type-topic exclusion applied to a raw
 *     get_topic_names_and_types() result.
 */

#include "../ros2_topic_subscription.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ros2_streamer;

// --- DesiredTopicsSlot ---

TEST(DesiredTopicsSlotTest, TakeOnEmptySlotReturnsNullopt) {
  DesiredTopicsSlot slot;
  EXPECT_FALSE(slot.take().has_value());
}

TEST(DesiredTopicsSlotTest, TakeReturnsWhatWasSetAndResetsSlot) {
  DesiredTopicsSlot slot;
  slot.set({"/a", "/b"});

  auto first = slot.take();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, (std::set<std::string>{"/a", "/b"}));

  // The slot is empty again after take() — a second drain finds nothing new.
  EXPECT_FALSE(slot.take().has_value());
}

TEST(DesiredTopicsSlotTest, SetIsSelfCoalescingNotQueued) {
  DesiredTopicsSlot slot;
  slot.set({"/a"});
  slot.set({"/b"});
  slot.set({"/c"});

  // Only ONE drain is needed to see the latest write — intermediate writes never queue.
  auto result = slot.take();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, (std::set<std::string>{"/c"}));
  EXPECT_FALSE(slot.take().has_value());
}

TEST(DesiredTopicsSlotTest, LastWriteFromAnotherThreadWins) {
  DesiredTopicsSlot slot;

  // Simulate the host GUI thread issuing a burst of set_active_topics calls while the
  // poll thread hasn't drained yet — only the LAST write before the eventual take()
  // may be observed.
  std::thread writer([&slot] {
    for (int i = 0; i < 100; ++i) {
      slot.set({"/topic_" + std::to_string(i)});
    }
  });
  writer.join();

  auto result = slot.take();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, (std::set<std::string>{"/topic_99"}));
}

// --- computeRos2SubscriptionDiff ---

TEST(ComputeRos2SubscriptionDiffTest, EmptyEverythingIsANoOp) {
  auto diff = computeRos2SubscriptionDiff({}, {}, {});
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, SubscribesNewlyDesiredDiscoveredTopic) {
  const std::set<std::string> current;  // nothing subscribed yet
  const std::map<std::string, std::string> subscribable{{"/camera/image", "sensor_msgs/msg/Image"}};
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  ASSERT_EQ(diff.to_subscribe.size(), 1U);
  EXPECT_EQ(diff.to_subscribe[0], "/camera/image");
}

TEST(ComputeRos2SubscriptionDiffTest, UnsubscribesNoLongerDesiredTopic) {
  const std::set<std::string> current{"/camera/image"};
  const std::map<std::string, std::string> subscribable{{"/camera/image", "sensor_msgs/msg/Image"}};
  const std::set<std::string> desired;  // host paused everything

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], "/camera/image");
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, AlreadySubscribedDesiredTopicIsUntouched) {
  const std::set<std::string> current{"/camera/image"};
  const std::map<std::string, std::string> subscribable{{"/camera/image", "sensor_msgs/msg/Image"}};
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, DesiredTopicNotYetDiscoveredIsIgnored) {
  // /odom is desired but not (yet) discovered on the ROS graph — nothing to
  // subscribe to; the caller is expected to remember it (last_applied_desired_)
  // and retry once the topic becomes discoverable.
  const std::set<std::string> current;
  const std::map<std::string, std::string> subscribable{{"/camera/image", "sensor_msgs/msg/Image"}};
  const std::set<std::string> desired{"/odom"};

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, DiscoveredButNotDesiredTopicIsNotSubscribed) {
  // /odom is discovered (and passes the filter) but the host never asked for it.
  const std::set<std::string> current;
  const std::map<std::string, std::string> subscribable{{"/odom", "nav_msgs/msg/Odometry"}};
  const std::set<std::string> desired;

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, SubscribedTopicNoLongerDiscoveredIsUnsubscribed) {
  // /camera/image dropped out of the discovered set (publisher went away) but the
  // subscription bookkeeping hasn't caught up yet — still must be dropped, even
  // though it is still nominally desired.
  const std::set<std::string> current{"/camera/image"};
  const std::map<std::string, std::string> subscribable;  // no longer discovered
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], "/camera/image");
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(ComputeRos2SubscriptionDiffTest, MixedAddAndDropInOneDiff) {
  // Currently subscribed: /a, /b. Desired now: /b, /c (both discovered).
  // Expect: unsubscribe /a (dropped), subscribe /c (newly added).
  const std::set<std::string> current{"/a", "/b"};
  const std::map<std::string, std::string> subscribable{
      {"/a", "std_msgs/msg/String"}, {"/b", "std_msgs/msg/String"}, {"/c", "std_msgs/msg/String"}};
  const std::set<std::string> desired{"/b", "/c"};

  auto diff = computeRos2SubscriptionDiff(current, subscribable, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], "/a");
  ASSERT_EQ(diff.to_subscribe.size(), 1U);
  EXPECT_EQ(diff.to_subscribe[0], "/c");
}

// --- passesAdvertiseFilter ---

TEST(PassesAdvertiseFilterTest, EmptySelectionAdvertisesEverything) {
  EXPECT_TRUE(passesAdvertiseFilter("/any/topic", {}));
}

TEST(PassesAdvertiseFilterTest, NonEmptySelectionRestrictsToSelection) {
  std::vector<std::pair<std::string, std::string>> selection{{"/camera/image", "sensor_msgs/msg/Image"}};
  EXPECT_TRUE(passesAdvertiseFilter("/camera/image", selection));
  EXPECT_FALSE(passesAdvertiseFilter("/odom", selection));
}

// --- filteredDiscoveredTopics: the filter bounds SUBSCRIPTIONS, not just advertising ---

TEST(FilteredDiscoveredTopicsTest, FilteredOutDesiredTopicProducesNoSubscription) {
  std::map<std::string, std::string> discovered{
      {"/camera/image", "sensor_msgs/msg/Image"}, {"/odom", "nav_msgs/msg/Odometry"}};
  std::vector<std::pair<std::string, std::string>> selection{{"/camera/image", "sensor_msgs/msg/Image"}};

  const auto subscribable = filteredDiscoveredTopics(discovered, selection);
  ASSERT_EQ(subscribable.size(), 1U);
  EXPECT_EQ(subscribable.at("/camera/image"), "sensor_msgs/msg/Image");

  // A (stale) desired set naming the filtered-out topic must not subscribe it —
  // the filter is explicit user configuration and outranks a leftover reference.
  const auto diff = computeRos2SubscriptionDiff({}, subscribable, {"/camera/image", "/odom"});
  EXPECT_EQ(diff.to_subscribe, (std::vector<std::string>{"/camera/image"}));
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(FilteredDiscoveredTopicsTest, LiveSubscriptionToNewlyFilteredTopicIsDropped) {
  std::map<std::string, std::string> discovered{{"/odom", "nav_msgs/msg/Odometry"}};
  std::vector<std::pair<std::string, std::string>> selection{{"/camera/image", "sensor_msgs/msg/Image"}};

  const auto subscribable = filteredDiscoveredTopics(discovered, selection);
  EXPECT_TRUE(subscribable.empty());

  const std::set<std::string> current{"/odom"};
  const auto diff = computeRos2SubscriptionDiff(current, subscribable, {"/odom"});
  EXPECT_EQ(diff.to_unsubscribe, (std::vector<std::string>{"/odom"}));
  EXPECT_TRUE(diff.to_subscribe.empty());
}

// --- splitTopicTypes ---

TEST(SplitTopicTypesTest, SingleTypeTopicsPassThrough) {
  std::map<std::string, std::vector<std::string>> raw{
      {"/odom", {"nav_msgs/msg/Odometry"}}, {"/tf", {"tf2_msgs/msg/TFMessage"}}};

  const auto split = splitTopicTypes(raw);
  EXPECT_TRUE(split.multi_type.empty());
  ASSERT_EQ(split.single_type.size(), 2U);
  EXPECT_EQ(split.single_type.at("/odom"), "nav_msgs/msg/Odometry");
  EXPECT_EQ(split.single_type.at("/tf"), "tf2_msgs/msg/TFMessage");
}

TEST(SplitTopicTypesTest, MultiTypeTopicsAreExcludedAndReported) {
  std::map<std::string, std::vector<std::string>> raw{
      {"/weird", {"pkg_a/msg/Foo", "pkg_b/msg/Bar"}}, {"/odom", {"nav_msgs/msg/Odometry"}}};

  const auto split = splitTopicTypes(raw);
  ASSERT_EQ(split.multi_type.size(), 1U);
  EXPECT_EQ(split.multi_type[0], "/weird");
  ASSERT_EQ(split.single_type.size(), 1U);
  EXPECT_EQ(split.single_type.at("/odom"), "nav_msgs/msg/Odometry");
}

TEST(SplitTopicTypesTest, EmptyTypesListIsSkipped) {
  std::map<std::string, std::vector<std::string>> raw{{"/nothing", {}}};

  const auto split = splitTopicTypes(raw);
  EXPECT_TRUE(split.multi_type.empty());
  EXPECT_TRUE(split.single_type.empty());
}

}  // namespace
