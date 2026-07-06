#pragma once

/// @file ros2_topic_subscription.hpp
/// @brief ROS-free control-plane logic for demand-driven per-topic subscription
///  (pj.topic_subscription.v1), factored out of ros2_stream_plugin_distro so it
///  is unit-testable WITHOUT rclcpp — the per-distro inner target only builds
///  inside a sourced ROS 2 environment (PJ_BUILD_ROS2_DISTRO), but this header
///  has no ROS dependency and is exercised by a plain gtest target that always
///  builds (see tests/ros2_topic_subscription_test.cpp and CMakeLists.txt).
///
/// Twin of data_stream_foxglove_bridge/foxglove_topic_subscription.hpp — the
/// Foxglove bridge's identical control plane for the same SDK contract — same
/// shape, adapted to ROS 2's topic model: subscriptions are keyed directly by
/// topic NAME (there is no channel-id indirection like Foxglove's per-
/// connection channel ids), and the "advertised" universe is the streamer's
/// OWN discovery scan (get_topic_names_and_types()) rather than a
/// server-pushed advertise message.

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ros2_streamer {

/// Mutex-protected "latest wins" slot for the host's desired active-topic set
/// (the payload of pj.topic_subscription.v1's set_active_topics). Twin of
/// PJ::FoxgloveProtocol::DesiredTopicsSlot — identical semantics, duplicated
/// here so data_stream_ros2 does not depend on data_stream_foxglove_bridge.
///
/// set_active_topics() is entered on the HOST GUI thread; this is NOT a command
/// queue — every call is a full, declarative replacement of the desired set, so
/// only the MOST RECENT write matters. The poll thread drains it with take(),
/// which atomically hands over whatever is pending and resets the slot to empty
/// (nullopt), so a poll pass that finds nothing new is a cheap no-op.
class DesiredTopicsSlot {
 public:
  /// [any thread] Overwrite the desired set. Always replaces, never queues.
  void set(std::set<std::string> topics) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_ = std::move(topics);
  }

  /// [poll thread] Take whatever is pending, resetting the slot to empty.
  /// Returns nullopt if nothing was written since the last take().
  [[nodiscard]] std::optional<std::set<std::string>> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::set<std::string>> result;
    result.swap(slot_);
    return result;
  }

 private:
  std::mutex mutex_;
  std::optional<std::set<std::string>> slot_;
};

/// Result of computeRos2SubscriptionDiff: which live subscriptions to drop and
/// which discoverable topics to newly subscribe, to reconcile the source's
/// current subscriptions with a desired active-topic set.
struct SubscriptionDiff {
  std::vector<std::string> to_unsubscribe;  ///< topic names to drop.
  std::vector<std::string> to_subscribe;    ///< topic names to newly subscribe (type resolved via `subscribable`).
};

/// Diff the source's live subscriptions against a desired active-topic set —
/// declarative and complete, exactly as delivered by pj.topic_subscription.v1.
/// Pure function (no I/O, no side effects): the demand-subscription apply
/// logic in Ros2StreamSource::onPoll is unit-testable through this without a
/// live ROS graph.
///
/// @param current_subscriptions  topic names the source is currently
///        subscribed to (the keys of `subscriptions_`).
/// @param subscribable  topic -> type, restricted to topics BOTH currently
///        discovered on the ROS graph AND passing the advertise filter (see
///        filteredDiscoveredTopics). A desired topic with no entry here
///        cannot be subscribed yet — the caller is expected to remember it
///        (in last_applied_desired_) and retry once/if the topic becomes
///        discoverable.
/// @param desired_topics  the full active-topic set the host wants live right now.
[[nodiscard]] inline SubscriptionDiff computeRos2SubscriptionDiff(
    const std::set<std::string>& current_subscriptions, const std::map<std::string, std::string>& subscribable,
    const std::set<std::string>& desired_topics) {
  SubscriptionDiff diff;

  for (const auto& topic : current_subscriptions) {
    const bool keep = desired_topics.count(topic) > 0 && subscribable.count(topic) > 0;
    if (!keep) {
      diff.to_unsubscribe.push_back(topic);
    }
  }

  for (const auto& [topic, type] : subscribable) {
    (void)type;
    if (desired_topics.count(topic) > 0 && current_subscriptions.count(topic) == 0) {
      diff.to_subscribe.push_back(topic);
    }
  }

  return diff;
}

/// Whether `topic` should be advertised/subscribed under an (optional) filter —
/// the dialog's saved topic selection, repurposed in demand mode as an
/// allow-list rather than a subscribe list. An EMPTY selection means "no
/// filter, advertise/subscribe everything" (the default when nothing is
/// selected). Mirrors PJ::FoxgloveProtocol::passesAdvertiseFilter.
[[nodiscard]] inline bool passesAdvertiseFilter(
    const std::string& topic, const std::vector<std::pair<std::string, std::string>>& selection) {
  return selection.empty() ||
         std::any_of(selection.begin(), selection.end(), [&](const auto& sel) { return sel.first == topic; });
}

/// The topic -> type map computeRos2SubscriptionDiff() reconciles against:
/// discovered topics only, restricted by the SAME filter buildAvailableTopics
/// applies when advertising. Advertise and subscribe stay symmetric — a topic
/// the filter hides from the host is never subscribed either, even if a
/// (stale) desired set names it: the filter is explicit user configuration
/// and outranks a leftover layout reference. Mirrors
/// PJ::FoxgloveProtocol::filteredAdvertisedTopics.
[[nodiscard]] inline std::map<std::string, std::string> filteredDiscoveredTopics(
    const std::map<std::string, std::string>& discovered,
    const std::vector<std::pair<std::string, std::string>>& selection) {
  std::map<std::string, std::string> out;
  for (const auto& [topic, type] : discovered) {
    if (passesAdvertiseFilter(topic, selection)) {
      out[topic] = type;
    }
  }
  return out;
}

/// Partition a raw get_topic_names_and_types() result into single-type topics
/// (name -> its one type) and the names of topics currently published under
/// more than one type. CDR deserialization is inherently type-specific, so a
/// multi-type topic cannot be advertised/subscribed without an arbitrary type
/// pick; the caller is expected to skip it and warn once.
struct TopicTypeSplit {
  std::map<std::string, std::string> single_type;
  std::vector<std::string> multi_type;
};

[[nodiscard]] inline TopicTypeSplit splitTopicTypes(const std::map<std::string, std::vector<std::string>>& raw) {
  TopicTypeSplit out;
  for (const auto& [name, types] : raw) {
    if (types.empty()) {
      continue;
    }
    if (types.size() > 1) {
      out.multi_type.push_back(name);
      continue;
    }
    out.single_type[name] = types.front();
  }
  return out;
}

}  // namespace ros2_streamer
