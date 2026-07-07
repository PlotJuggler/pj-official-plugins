/**
 * @file pj_bridge_topic_subscription_test.cpp
 * @brief Unit tests for the PJ Bridge demand-driven per-topic subscription
 *  control plane (pj.topic_subscription.v1), factored into
 *  pj_bridge_topic_subscription.hpp so it is testable without a live WebSocket
 *  or host:
 *
 *   - DesiredTopicsSlot: the mutex-protected "latest wins" slot written by
 *     set_active_topics (host GUI thread) and drained by onPoll (poll thread).
 *   - TextFrameQueue: the socket-thread → poll-thread handoff that keeps all
 *     parser-binding work on the poll thread (the demand-mode race fix).
 *   - computeSubscriptionDiff: the pure additive-subscribe / unsubscribe diff.
 *   - passesAdvertiseFilter / filteredAdvertisedTopics: the dialog-selection-as-
 *     advertise-filter predicate and the filter-bounded advertised set.
 *   - parseTopicEntry / applyAdvertiseDelta: the get_topics / topics_changed
 *     catalog update, including the include_schemas graceful-degradation path.
 */

#include "../pj_bridge_topic_subscription.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace PJ::BridgeProtocol;

// --- DesiredTopicsSlot ---

TEST(BridgeDesiredTopicsSlotTest, TakeOnEmptySlotReturnsNullopt) {
  DesiredTopicsSlot slot;
  EXPECT_FALSE(slot.take().has_value());
}

TEST(BridgeDesiredTopicsSlotTest, TakeReturnsWhatWasSetAndResetsSlot) {
  DesiredTopicsSlot slot;
  slot.set({"/a", "/b"});

  auto first = slot.take();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, (std::set<std::string>{"/a", "/b"}));

  // The slot is empty again after take() — a second drain finds nothing new.
  EXPECT_FALSE(slot.take().has_value());
}

TEST(BridgeDesiredTopicsSlotTest, SetIsSelfCoalescingNotQueued) {
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

TEST(BridgeDesiredTopicsSlotTest, LastWriteFromAnotherThreadWins) {
  DesiredTopicsSlot slot;

  // Simulate the host GUI thread issuing a burst of set_active_topics calls while the
  // poll thread hasn't drained yet — only the LAST write before the eventual take() wins.
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

// --- TextFrameQueue: socket-thread → poll-thread handoff (bindings stay on the poll thread) ---

TEST(TextFrameQueueTest, DrainOnEmptyQueueReturnsEmpty) {
  TextFrameQueue q;
  EXPECT_TRUE(q.drain().empty());
}

TEST(TextFrameQueueTest, PreservesArrivalOrder) {
  TextFrameQueue q;
  q.push("first");
  q.push("second");
  q.push("third");

  auto drained = q.drain();
  ASSERT_EQ(drained.size(), 3U);
  EXPECT_EQ(drained.front(), "first");
  drained.pop();
  EXPECT_EQ(drained.front(), "second");
  drained.pop();
  EXPECT_EQ(drained.front(), "third");

  // A second drain is a cheap no-op — the queue was swapped empty.
  EXPECT_TRUE(q.drain().empty());
}

TEST(TextFrameQueueTest, ConcurrentWriterFramesAllArriveInOrder) {
  TextFrameQueue q;
  constexpr int kCount = 1000;

  // The socket thread pushes; the poll thread (this thread) drains in batches.
  // Every pushed frame must be observed exactly once, in push order.
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < kCount; ++i) {
      q.push(std::to_string(i));
    }
    done = true;
  });

  std::vector<std::string> received;
  while (received.size() < static_cast<size_t>(kCount)) {
    auto batch = q.drain();
    while (!batch.empty()) {
      received.push_back(std::move(batch.front()));
      batch.pop();
    }
    if (!done && received.empty()) {
      std::this_thread::yield();
    }
  }
  writer.join();

  ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(received[static_cast<size_t>(i)], std::to_string(i));
  }
}

// --- computeSubscriptionDiff ---

TEST(BridgeComputeSubscriptionDiffTest, EmptyEverythingIsANoOp) {
  auto diff = computeSubscriptionDiff({}, {}, {});
  EXPECT_TRUE(diff.to_subscribe.empty());
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(BridgeComputeSubscriptionDiffTest, SubscribesNewlyDesiredAdvertisedTopic) {
  auto diff = computeSubscriptionDiff(/*applied=*/{}, /*advertised=*/{"/camera/image"}, /*desired=*/{"/camera/image"});
  EXPECT_TRUE(diff.to_unsubscribe.empty());
  EXPECT_EQ(diff.to_subscribe, (std::vector<std::string>{"/camera/image"}));
}

TEST(BridgeComputeSubscriptionDiffTest, UnsubscribesNoLongerDesiredTopic) {
  // Host paused everything: desired is empty, so the live subscription is dropped.
  auto diff = computeSubscriptionDiff(/*applied=*/{"/camera/image"}, /*advertised=*/{"/camera/image"}, /*desired=*/{});
  EXPECT_EQ(diff.to_unsubscribe, (std::vector<std::string>{"/camera/image"}));
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(BridgeComputeSubscriptionDiffTest, AlreadySubscribedDesiredTopicIsUntouched) {
  // Empty-send suppression: no add, no drop → both lists empty, so no wire command.
  auto diff = computeSubscriptionDiff(/*applied=*/{"/camera/image"}, /*advertised=*/{"/camera/image"},
                                      /*desired=*/{"/camera/image"});
  EXPECT_TRUE(diff.to_subscribe.empty());
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(BridgeComputeSubscriptionDiffTest, DesiredTopicNotYetAdvertisedStaysPending) {
  // /odom is desired but the server has not advertised it — nothing to subscribe;
  // the caller keeps it in `desired` and retries when topics_changed adds it.
  auto diff = computeSubscriptionDiff(/*applied=*/{}, /*advertised=*/{"/camera/image"}, /*desired=*/{"/odom"});
  EXPECT_TRUE(diff.to_subscribe.empty());
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(BridgeComputeSubscriptionDiffTest, SubscribedTopicNoLongerAdvertisedIsUnsubscribed) {
  // The server dropped /camera/image from its advertised set (topics_changed.removed)
  // but the subscription bookkeeping hasn't caught up — still must be dropped.
  auto diff = computeSubscriptionDiff(/*applied=*/{"/camera/image"}, /*advertised=*/{}, /*desired=*/{"/camera/image"});
  EXPECT_EQ(diff.to_unsubscribe, (std::vector<std::string>{"/camera/image"}));
  EXPECT_TRUE(diff.to_subscribe.empty());
}

TEST(BridgeComputeSubscriptionDiffTest, MixedAddAndDropInOneDiff) {
  // Applied: /a, /b. Desired: /b, /c. Expect: drop /a, add /c.
  auto diff = computeSubscriptionDiff(
      /*applied=*/{"/a", "/b"}, /*advertised=*/{"/a", "/b", "/c"}, /*desired=*/{"/b", "/c"});
  EXPECT_EQ(diff.to_unsubscribe, (std::vector<std::string>{"/a"}));
  EXPECT_EQ(diff.to_subscribe, (std::vector<std::string>{"/c"}));
}

TEST(BridgeComputeSubscriptionDiffTest, PerTopicFailureRetriesOnceRemovedFromApplied) {
  // The server rejected /camera/image (per-topic failure) and forgets it, so the
  // caller removes it from `applied`. It is still desired AND still advertised, so
  // the NEXT reconcile re-proposes it — this models the "retry on advertise change".
  const std::set<std::string> advertised{"/camera/image"};
  const std::set<std::string> desired{"/camera/image"};

  // Right after the failure was stripped from `applied`:
  auto diff = computeSubscriptionDiff(/*applied=*/{}, advertised, desired);
  EXPECT_EQ(diff.to_subscribe, (std::vector<std::string>{"/camera/image"}));
}

// --- passesAdvertiseFilter ---

TEST(BridgePassesAdvertiseFilterTest, EmptySelectionAdvertisesEverything) {
  EXPECT_TRUE(passesAdvertiseFilter("/any/topic", {}));
}

TEST(BridgePassesAdvertiseFilterTest, NonEmptySelectionRestrictsToSelection) {
  const std::vector<std::string> selection{"/camera/image"};
  EXPECT_TRUE(passesAdvertiseFilter("/camera/image", selection));
  EXPECT_FALSE(passesAdvertiseFilter("/odom", selection));
}

// --- filteredAdvertisedTopics: the filter bounds SUBSCRIPTIONS, not just advertising ---

TopicInfo topic(const std::string& name, const std::string& type = "pkg/Msg") {
  TopicInfo t;
  t.name = name;
  t.encoding = "cdr";
  t.schema_name = type;
  t.schema = "float64 value";
  return t;
}

TEST(BridgeFilteredAdvertisedTopicsTest, FilteredOutDesiredTopicProducesNoSubscription) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/camera/image"] = topic("/camera/image");
  advertised["/odom"] = topic("/odom");

  const std::vector<std::string> selection{"/camera/image"};  // /odom filtered out
  const auto advertised_set = filteredAdvertisedTopics(advertised, selection);
  EXPECT_EQ(advertised_set, (std::set<std::string>{"/camera/image"}));

  // A (stale) desired set naming the filtered-out topic must not subscribe it.
  const auto diff = computeSubscriptionDiff({}, advertised_set, {"/camera/image", "/odom"});
  EXPECT_EQ(diff.to_subscribe, (std::vector<std::string>{"/camera/image"}));
  EXPECT_TRUE(diff.to_unsubscribe.empty());
}

TEST(BridgeFilteredAdvertisedTopicsTest, EmptySelectionAdvertisesAll) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a");
  advertised["/b"] = topic("/b");
  EXPECT_EQ(filteredAdvertisedTopics(advertised, {}), (std::set<std::string>{"/a", "/b"}));
}

// --- parseTopicEntry: get_topics / topics_changed.added entry parsing ---

TEST(BridgeParseTopicEntryTest, WrongTypedFieldsDegradeToAbsentInsteadOfThrowing) {
  // Off-the-wire malice/corruption: present-but-wrong-typed keys. json::value()
  // would THROW here and escape into the poll loop; stringField must not.
  const auto entry = nlohmann::json::parse(R"({"name": 42})");
  EXPECT_EQ(parseTopicEntry(entry), std::nullopt);

  const auto entry2 = nlohmann::json::parse(R"({"name": "/a", "type": 7, "definition": ["x"]})");
  const auto info = parseTopicEntry(entry2);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->name, "/a");
  EXPECT_TRUE(info->schema_name.empty());
  EXPECT_TRUE(info->schema.empty());
}

TEST(BridgeRetypedTopicsTest, AppliedTopicWithChangedInfoIsReported) {
  std::map<std::string, PJ::BridgeProtocol::TopicInfo> before;
  before["/a"] = {"/a", "cdr", "pkg/msg/Old", "old-schema"};
  before["/b"] = {"/b", "cdr", "pkg/msg/Same", "same"};
  auto after = before;
  after["/a"].schema_name = "pkg/msg/New";
  after["/a"].schema = "new-schema";

  // /a changed and is applied -> re-subscribe it; /b unchanged -> untouched;
  // a changed-but-not-applied topic must not appear.
  after["/c"] = {"/c", "cdr", "pkg/msg/C", "c"};
  const auto retyped = retypedTopics(before, after, {"/a", "/b"});
  EXPECT_EQ(retyped, std::vector<std::string>{"/a"});
}

TEST(BridgeRetypedTopicsTest, AddedAndRemovedTopicsAreNotRetyped) {
  std::map<std::string, PJ::BridgeProtocol::TopicInfo> before;
  before["/gone"] = {"/gone", "cdr", "pkg/msg/G", "g"};
  std::map<std::string, PJ::BridgeProtocol::TopicInfo> after;
  after["/new"] = {"/new", "cdr", "pkg/msg/N", "n"};
  EXPECT_TRUE(retypedTopics(before, after, {"/gone", "/new"}).empty());
}

TEST(BridgeProtocolVersionTest, OnlyAKnownNewerVersionIsUnsupported) {
  using PJ::BridgeProtocol::kSupportedProtocolVersion;
  using PJ::BridgeProtocol::unsupportedProtocolVersion;
  // Same version: fine.
  EXPECT_EQ(unsupportedProtocolVersion(nlohmann::json{{"protocol_version", 1}}, 1), std::nullopt);
  // Newer version: the one hard-fail.
  EXPECT_EQ(unsupportedProtocolVersion(nlohmann::json{{"protocol_version", 2}}, 1), std::optional<int>(2));
  // Absent (pre-versioning server) or wrong-typed: tolerated.
  EXPECT_EQ(unsupportedProtocolVersion(nlohmann::json::object(), 1), std::nullopt);
  EXPECT_EQ(unsupportedProtocolVersion(nlohmann::json{{"protocol_version", "2"}}, 1), std::nullopt);
  EXPECT_EQ(kSupportedProtocolVersion, 1);
}

TEST(BridgeServerInfoTest, ParsesCapabilitiesAndToleratesAbsence) {
  using PJ::BridgeProtocol::parseServerInfo;
  const auto full = parseServerInfo(
      nlohmann::json::parse(
          R"({"server": {"name": "pj_bridge", "version": "0.6.0",
                     "capabilities": ["include_schemas", "latched_replay", 7]}})"));
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(full->name, "pj_bridge");
  EXPECT_EQ(full->version, "0.6.0");
  EXPECT_TRUE(full->hasCapability("include_schemas"));
  EXPECT_TRUE(full->hasCapability("latched_replay"));
  EXPECT_FALSE(full->hasCapability("7"));  // non-string entries dropped

  // Pre-capability server: no object at all.
  EXPECT_EQ(parseServerInfo(nlohmann::json::object()), std::nullopt);
  // Wrong-typed object tolerated as absent.
  EXPECT_EQ(parseServerInfo(nlohmann::json{{"server", "pj_bridge"}}), std::nullopt);
}

TEST(BridgeFailureIsStaleTest, StaleOnlyWhenIdsDisagree) {
  const std::map<std::string, std::string> last_ids{{"/a", "req-2"}};
  // Failure from the superseded req-1: stale -> ignore, applied_ keeps truth.
  EXPECT_TRUE(failureIsStale(last_ids, "/a", "req-1"));
  // Failure from the live request: act on it.
  EXPECT_FALSE(failureIsStale(last_ids, "/a", "req-2"));
  // No id echoed (older/test servers): never stale.
  EXPECT_FALSE(failureIsStale(last_ids, "/a", ""));
  // Topic we never subscribed (or already cleaned up): not stale.
  EXPECT_FALSE(failureIsStale(last_ids, "/unknown", "req-9"));
}

TEST(BridgeParseTopicEntryTest, FullEntryWithSchemaFields) {
  const auto entry = nlohmann::json{
      {"name", "/camera/image"},
      {"type", "sensor_msgs/msg/CompressedImage"},
      {"encoding", "cdr"},
      {"definition", "uint8[] data"},
  };
  const auto info = parseTopicEntry(entry);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->name, "/camera/image");
  EXPECT_EQ(info->schema_name, "sensor_msgs/msg/CompressedImage");
  EXPECT_EQ(info->encoding, "cdr");
  EXPECT_EQ(info->schema, "uint8[] data");
}

TEST(BridgeParseTopicEntryTest, NameAndTypeOnlyGracefulDegradation) {
  // Old server (ignores include_schemas) or a topic whose schema extraction failed:
  // encoding/definition absent → left empty, but the topic is still parsed.
  const auto entry = nlohmann::json{{"name", "/odom"}, {"type", "nav_msgs/msg/Odometry"}};
  const auto info = parseTopicEntry(entry);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->name, "/odom");
  EXPECT_EQ(info->schema_name, "nav_msgs/msg/Odometry");
  EXPECT_TRUE(info->encoding.empty());
  EXPECT_TRUE(info->schema.empty());
}

TEST(BridgeParseTopicEntryTest, ExplicitSchemaNameWinsOverType) {
  const auto entry = nlohmann::json{{"name", "/t"}, {"type", "T"}, {"schema_name", "pkg/T"}};
  const auto info = parseTopicEntry(entry);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->schema_name, "pkg/T");
}

TEST(BridgeParseTopicEntryTest, MissingNameIsRejected) {
  EXPECT_FALSE(parseTopicEntry(nlohmann::json{{"type", "T"}}).has_value());
  EXPECT_FALSE(parseTopicEntry(nlohmann::json{{"name", ""}, {"type", "T"}}).has_value());
}

TEST(BridgeParseTopicEntryTest, NonObjectIsRejected) {
  EXPECT_FALSE(parseTopicEntry(nlohmann::json("just a string")).has_value());
  EXPECT_FALSE(parseTopicEntry(nlohmann::json::array()).has_value());
}

// --- applyAdvertiseDelta: catalog update from topics_changed ---

TEST(BridgeApplyAdvertiseDeltaTest, AddsNewTopicsAndReportsChanged) {
  std::map<std::string, TopicInfo> advertised;
  const bool changed = applyAdvertiseDelta(advertised, {topic("/a"), topic("/b")}, {});
  EXPECT_TRUE(changed);
  EXPECT_EQ(advertised.size(), 2U);
  EXPECT_TRUE(advertised.count("/a"));
  EXPECT_TRUE(advertised.count("/b"));
}

TEST(BridgeApplyAdvertiseDeltaTest, ReAddingIdenticalEntryIsNotAChange) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a");
  const bool changed = applyAdvertiseDelta(advertised, {topic("/a")}, {});
  EXPECT_FALSE(changed);
  EXPECT_EQ(advertised.size(), 1U);
}

TEST(BridgeApplyAdvertiseDeltaTest, ReAddingWithNewSchemaReplacesAndReportsChanged) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a", "OldType");
  const bool changed = applyAdvertiseDelta(advertised, {topic("/a", "NewType")}, {});
  EXPECT_TRUE(changed);
  EXPECT_EQ(advertised.at("/a").schema_name, "NewType");
}

TEST(BridgeApplyAdvertiseDeltaTest, RemovesTopicsAndReportsChanged) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a");
  advertised["/b"] = topic("/b");
  const bool changed = applyAdvertiseDelta(advertised, {}, {"/a"});
  EXPECT_TRUE(changed);
  EXPECT_EQ(advertised.size(), 1U);
  EXPECT_FALSE(advertised.count("/a"));
}

TEST(BridgeApplyAdvertiseDeltaTest, RemovingUnknownTopicIsNotAChange) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a");
  const bool changed = applyAdvertiseDelta(advertised, {}, {"/ghost"});
  EXPECT_FALSE(changed);
  EXPECT_EQ(advertised.size(), 1U);
}

TEST(BridgeApplyAdvertiseDeltaTest, MixedAddAndRemoveInOneDelta) {
  std::map<std::string, TopicInfo> advertised;
  advertised["/a"] = topic("/a");
  advertised["/b"] = topic("/b");
  const bool changed = applyAdvertiseDelta(advertised, {topic("/c")}, {"/a"});
  EXPECT_TRUE(changed);
  EXPECT_EQ(advertised.size(), 2U);
  EXPECT_TRUE(advertised.count("/b"));
  EXPECT_TRUE(advertised.count("/c"));
  EXPECT_FALSE(advertised.count("/a"));
}

}  // namespace
