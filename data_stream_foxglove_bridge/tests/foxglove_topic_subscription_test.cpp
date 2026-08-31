/**
 * @file foxglove_topic_subscription_test.cpp
 * @brief Unit tests for the demand-driven per-topic subscription control plane
 *  (pj.topic_subscription.v1), factored into foxglove_topic_subscription.hpp so
 *  it is testable without a live WebSocket or host:
 *
 *   - DesiredTopicsSlot: the mutex-protected "latest wins" slot written by
 *     set_active_topics (host GUI thread) and drained by onPoll (poll thread).
 *   - computeSubscriptionDiff: the pure subscribe/unsubscribe diff between the
 *     source's live subscriptions and the host's desired active-topic set.
 *   - passesAdvertiseFilter: the dialog-selection-as-advertise-filter predicate.
 */

#include "../foxglove_topic_subscription.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace PJ::FoxgloveProtocol;

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

// --- computeSubscriptionDiff ---

TEST(ComputeSubscriptionDiffTest, EmptyEverythingIsANoOp) {
  auto diff = computeSubscriptionDiff({}, {}, {});
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe_channel_ids.empty());
}

TEST(ComputeSubscriptionDiffTest, SubscribesNewlyDesiredAdvertisedTopic) {
  const std::map<uint32_t, uint64_t> current;  // nothing subscribed yet
  const std::map<uint64_t, std::string> advertised{{100, "/camera/image"}};
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  ASSERT_EQ(diff.to_subscribe_channel_ids.size(), 1U);
  EXPECT_EQ(diff.to_subscribe_channel_ids[0], 100U);
}

TEST(ComputeSubscriptionDiffTest, UnsubscribesNoLongerDesiredTopic) {
  const std::map<uint32_t, uint64_t> current{{1, 100}};  // sub 1 -> channel 100
  const std::map<uint64_t, std::string> advertised{{100, "/camera/image"}};
  const std::set<std::string> desired;  // host paused everything

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], 1U);
  EXPECT_TRUE(diff.to_subscribe_channel_ids.empty());
}

TEST(ComputeSubscriptionDiffTest, AlreadySubscribedDesiredTopicIsUntouched) {
  const std::map<uint32_t, uint64_t> current{{1, 100}};
  const std::map<uint64_t, std::string> advertised{{100, "/camera/image"}};
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe_channel_ids.empty());
}

TEST(ComputeSubscriptionDiffTest, DesiredTopicWithNoAdvertisedChannelIsIgnored) {
  // /odom is desired but not (yet) in the advertised set — nothing to subscribe to;
  // the caller is expected to remember it and retry once the channel (re-)advertises.
  const std::map<uint32_t, uint64_t> current;
  const std::map<uint64_t, std::string> advertised{{100, "/camera/image"}};
  const std::set<std::string> desired{"/odom"};

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_TRUE(diff.to_subscribe_channel_ids.empty());
}

TEST(ComputeSubscriptionDiffTest, SubscribedChannelNoLongerAdvertisedIsUnsubscribed) {
  // channel 100 dropped out of the advertised set (server unadvertised it) but the
  // subscription bookkeeping hasn't caught up yet — still must be dropped.
  const std::map<uint32_t, uint64_t> current{{1, 100}};
  const std::map<uint64_t, std::string> advertised;  // 100 no longer advertised
  const std::set<std::string> desired{"/camera/image"};

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], 1U);
}

TEST(ComputeSubscriptionDiffTest, MixedAddAndDropInOneDiff) {
  // Currently subscribed: /a (sub 1 -> chan 10), /b (sub 2 -> chan 20).
  // Desired now: /b, /c. Expect: unsubscribe sub 1 (/a dropped), subscribe chan 30 (/c added).
  const std::map<uint32_t, uint64_t> current{{1, 10}, {2, 20}};
  const std::map<uint64_t, std::string> advertised{{10, "/a"}, {20, "/b"}, {30, "/c"}};
  const std::set<std::string> desired{"/b", "/c"};

  auto diff = computeSubscriptionDiff(current, advertised, desired);
  ASSERT_EQ(diff.to_unsubscribe.size(), 1U);
  EXPECT_EQ(diff.to_unsubscribe[0], 1U);
  ASSERT_EQ(diff.to_subscribe_channel_ids.size(), 1U);
  EXPECT_EQ(diff.to_subscribe_channel_ids[0], 30U);
}

// --- passesAdvertiseFilter ---

TEST(PassesAdvertiseFilterTest, EmptySelectionAdvertisesEverything) {
  EXPECT_TRUE(passesAdvertiseFilter("/any/topic", {}));
}

TEST(PassesAdvertiseFilterTest, NonEmptySelectionRestrictsToSelection) {
  std::vector<ChannelInfo> selection(1);
  selection[0].topic = "/camera/image";
  EXPECT_TRUE(passesAdvertiseFilter("/camera/image", selection));
  EXPECT_FALSE(passesAdvertiseFilter("/odom", selection));
}

// --- filteredAdvertisedTopics: the filter bounds SUBSCRIPTIONS, not just advertising ---

ChannelInfo supportedChannel(uint64_t id, const std::string& topic) {
  ChannelInfo ch;
  ch.id = id;
  ch.topic = topic;
  ch.encoding = "cdr";
  ch.schema_name = "pkg/Msg";
  ch.schema = "float64 value";
  ch.schema_encoding = "ros2msg";
  return ch;
}

TEST(FilteredAdvertisedTopicsTest, FilteredOutDesiredTopicProducesNoSubscription) {
  std::map<uint64_t, ChannelInfo> advertised;
  advertised[1] = supportedChannel(1, "/camera/image");
  advertised[2] = supportedChannel(2, "/odom");

  std::vector<ChannelInfo> selection(1);
  selection[0].topic = "/camera/image";  // /odom is filtered out of advertising

  const auto advertised_topics = filteredAdvertisedTopics(advertised, selection);
  ASSERT_EQ(advertised_topics.size(), 1U);
  EXPECT_EQ(advertised_topics.at(1), "/camera/image");

  // A (stale) desired set naming the filtered-out topic must not subscribe it —
  // the filter is explicit user configuration and outranks a leftover reference.
  const auto diff = computeSubscriptionDiff({}, advertised_topics, {"/camera/image", "/odom"});
  EXPECT_EQ(diff.to_subscribe_channel_ids, (std::vector<uint64_t>{1}));
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(FilteredAdvertisedTopicsTest, LiveSubscriptionToNewlyFilteredTopicIsDropped) {
  std::map<uint64_t, ChannelInfo> advertised;
  advertised[2] = supportedChannel(2, "/odom");

  std::vector<ChannelInfo> selection(1);
  selection[0].topic = "/camera/image";  // filter no longer includes /odom

  const auto advertised_topics = filteredAdvertisedTopics(advertised, selection);
  EXPECT_TRUE(advertised_topics.empty());

  const std::map<uint32_t, uint64_t> current = {{7, 2}};  // sub 7 -> /odom's channel
  const auto diff = computeSubscriptionDiff(current, advertised_topics, {"/odom"});
  EXPECT_EQ(diff.to_unsubscribe, (std::vector<uint32_t>{7}));
  EXPECT_TRUE(diff.to_subscribe_channel_ids.empty());
}

TEST(FilteredAdvertisedTopicsTest, UnsupportedChannelIsExcludedRegardlessOfFilter) {
  std::map<uint64_t, ChannelInfo> advertised;
  advertised[3] = supportedChannel(3, "/ok");
  advertised[4].id = 4;  // default-constructed → empty encoding → unsupported
  advertised[4].topic = "/unsupported";

  EXPECT_EQ(filteredAdvertisedTopics(advertised, {}).size(), 1U);
}

TEST(FilteredAdvertisedTopicsTest, JsonChannelIsAdvertised) {
  // LeRobot-style schemaless-typed json channel reaches the reconcilable map.
  std::map<uint64_t, ChannelInfo> advertised;
  advertised[5].id = 5;
  advertised[5].topic = "/observation/state";
  advertised[5].encoding = "json";
  advertised[5].schema_encoding = "jsonschema";
  advertised[5].schema = R"({"type":"object"})";

  const auto advertised_topics = filteredAdvertisedTopics(advertised, {});
  ASSERT_EQ(advertised_topics.size(), 1U);
  EXPECT_EQ(advertised_topics.at(5), "/observation/state");
}

}  // namespace
