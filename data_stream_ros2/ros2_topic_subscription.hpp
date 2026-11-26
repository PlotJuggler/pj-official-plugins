#pragma once

/// @file ros2_topic_subscription.hpp
/// ROS-free control plane for demand-driven per-topic subscription (pj.topic_subscription.v1), split out of
/// ros2_stream_plugin_distro (which only builds in a ROS 2 environment) so a plain gtest target can exercise it.
/// Twin of foxglove_topic_subscription.hpp adapted to ROS 2: subscriptions are keyed by topic name, and the
/// advertised universe is the streamer's own discovery scan (get_topic_names_and_types()) rather than a server push.

#include <algorithm>
#include <map>
#include <pj_always_included/locked_table.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ros2_streamer {

/// "Latest wins" slot for the host's desired active-topic set (set_active_topics, entered on the host GUI thread).
/// Not a command queue: every write is a full replacement, so only the most recent matters. The poll thread drains it
/// with take(), which hands over what is pending and resets the slot to nullopt.
using DesiredTopicsSlot = PJ::sdk::LatestValueSlot<std::set<std::string>>;

struct SubscriptionDiff {
  std::vector<std::string> to_unsubscribe;  ///< topic names to drop.
  std::vector<std::string> to_subscribe;    ///< topic names to newly subscribe (type resolved via `subscribable`).
};

/// Pure diff of live subscriptions against the desired active-topic set (declarative and complete).
///
/// A live subscription whose discovered type no longer matches the type it was created with lands in both lists:
/// CDR decoding is type-specific, so a re-typed topic must be recreated. The caller applies to_unsubscribe first.
///
/// @param current_subscriptions  topic -> the type each live subscription was created with (the source's
///        `subscription_types_`, in lockstep with `subscriptions_`).
/// @param subscribable  topic -> type, restricted to topics both discovered on the ROS graph and passing the advertise
///        filter (see filteredDiscoveredTopics). A desired topic with no entry cannot be subscribed yet; the caller
///        keeps it in last_applied_desired_ and retries when it becomes discoverable.
[[nodiscard]] inline SubscriptionDiff computeRos2SubscriptionDiff(
    const std::map<std::string, std::string>& current_subscriptions,
    const std::map<std::string, std::string>& subscribable, const std::set<std::string>& desired_topics) {
  SubscriptionDiff diff;

  for (const auto& [topic, subscribed_type] : current_subscriptions) {
    const auto it = subscribable.find(topic);
    const bool keep = desired_topics.count(topic) > 0 && it != subscribable.end() && it->second == subscribed_type;
    if (!keep) {
      diff.to_unsubscribe.push_back(topic);
    }
  }

  for (const auto& [topic, type] : subscribable) {
    if (desired_topics.count(topic) == 0) {
      continue;
    }
    const auto it = current_subscriptions.find(topic);
    if (it == current_subscriptions.end() || it->second != type) {
      diff.to_subscribe.push_back(topic);
    }
  }

  return diff;
}

/// The dialog's saved (name, type) selection acts in demand mode as an allow-list; an empty selection means no filter.
/// A transform type (PJ::AlwaysIncluded::matchesRos2) always passes.
[[nodiscard]] inline bool passesAdvertiseFilter(
    const std::string& topic, const std::string& type,
    const std::vector<std::pair<std::string, std::string>>& selection) {
  return PJ::AlwaysIncluded::matchesRos2(type) ||
         PJ::sdk::passesSelectionFilter(
             topic, selection, [](const auto& value) -> const std::string& { return value.first; });
}

/// Discovered topics restricted by the same filter buildAvailableTopics applies. Advertise and subscribe stay
/// symmetric: a topic the filter hides is never subscribed, even if a stale desired set names it (the filter is
/// explicit user configuration and outranks a leftover layout reference).
[[nodiscard]] inline std::map<std::string, std::string> filteredDiscoveredTopics(
    const std::map<std::string, std::string>& discovered,
    const std::vector<std::pair<std::string, std::string>>& selection) {
  std::map<std::string, std::string> out;
  for (const auto& [topic, type] : discovered) {
    if (passesAdvertiseFilter(topic, type, selection)) {
      out[topic] = type;
    }
  }
  return out;
}

/// Dialog table rows: topics whose name contains `filter` (ignoring case), plus every transform topic, which a filter
/// never hides and whose row is locked.
[[nodiscard]] inline PJ::AlwaysIncluded::TableRows topicRows(
    const std::map<std::string, std::string>& discovered, const std::string& filter) {
  const std::string needle = PJ::sdk::lowerAscii(filter);
  PJ::AlwaysIncluded::TableRows out;
  for (const auto& [name, type] : discovered) {
    const bool locked = PJ::AlwaysIncluded::matchesRos2(type);
    if (needle.empty() || locked || PJ::sdk::lowerAscii(name).find(needle) != std::string::npos) {
      out.push_back({{name, type}, locked});
    }
  }
  return out;
}

/// Splits a raw get_topic_names_and_types() result into single-type topics and the names published under more than one
/// type. CDR deserialization is type-specific, so a multi-type topic cannot be advertised or subscribed without an
/// arbitrary pick; the caller skips it and warns once.
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
