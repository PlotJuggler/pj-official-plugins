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
#include <pj_always_included/always_included.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
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
using DesiredTopicsSlot = PJ::sdk::LatestValueSlot<std::set<std::string>>;

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
/// A live subscription whose DISCOVERED type no longer matches the type it was
/// created with lands in BOTH lists (unsubscribe the stale one, subscribe the
/// new type) — CDR decoding is type-specific, so a re-typed topic must be
/// recreated, never silently kept. The caller applies to_unsubscribe first.
///
/// @param current_subscriptions  topic -> the type each live subscription was
///        CREATED with (the source's `subscription_types_`, lockstep with
///        `subscriptions_`).
/// @param subscribable  topic -> type, restricted to topics BOTH currently
///        discovered on the ROS graph AND passing the advertise filter (see
///        filteredDiscoveredTopics). A desired topic with no entry here
///        cannot be subscribed yet — the caller is expected to remember it
///        (in last_applied_desired_) and retry once/if the topic becomes
///        discoverable.
/// @param desired_topics  the full active-topic set the host wants live right now.
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

/// Whether `topic` should be advertised/subscribed under an (optional) filter —
/// the dialog's saved topic selection, repurposed in demand mode as an
/// allow-list rather than a subscribe list. An EMPTY selection means "no
/// filter, advertise/subscribe everything" (the default when nothing is
/// selected). Mirrors PJ::FoxgloveProtocol::passesAdvertiseFilter.
[[nodiscard]] inline bool passesAdvertiseFilter(
    const std::string& topic, const std::vector<std::pair<std::string, std::string>>& selection) {
  return PJ::sdk::passesSelectionFilter(
      topic, selection, [](const auto& value) -> const std::string& { return value.first; });
}

/// Whether a topic of ROS 2 type `type` is always subscribed, whatever the
/// selection and the host's desired set (transforms; see common/always_included).
/// Every rule of this plugin about forced topics goes through here.
[[nodiscard]] inline bool isAlwaysSubscribed(std::string_view type) {
  return PJ::AlwaysIncluded::matches(type, PJ::AlwaysIncluded::kRos2Encoding);
}

/// The discovered topics that are always subscribed. A forced type nobody
/// publishes is never invented.
[[nodiscard]] inline std::set<std::string> alwaysSubscribedTopics(
    const std::map<std::string, std::string>& discovered) {
  std::set<std::string> out;
  for (const auto& [topic, type] : discovered) {
    if (isAlwaysSubscribed(type)) {
      out.insert(topic);
    }
  }
  return out;
}

/// The topic -> type map computeRos2SubscriptionDiff() reconciles against:
/// discovered topics only, restricted by the SAME filter buildAvailableTopics
/// applies when advertising, which always-subscribed topics pass. Advertise and subscribe stay symmetric — a topic
/// the filter hides from the host is never subscribed either, even if a
/// (stale) desired set names it: the filter is explicit user configuration
/// and outranks a leftover layout reference. Mirrors
/// PJ::FoxgloveProtocol::filteredAdvertisedTopics.
[[nodiscard]] inline std::map<std::string, std::string> filteredDiscoveredTopics(
    const std::map<std::string, std::string>& discovered,
    const std::vector<std::pair<std::string, std::string>>& selection) {
  std::map<std::string, std::string> out;
  for (const auto& [topic, type] : discovered) {
    if (passesAdvertiseFilter(topic, selection) || isAlwaysSubscribed(type)) {
      out[topic] = type;
    }
  }
  return out;
}

// --- The dialog's topic table (ROS-free so it is unit-testable) ---

/// (topic, type) pairs in table order: the dialog's selection or its visible rows.
using TopicList = std::vector<std::pair<std::string, std::string>>;

/// The rows the table shows: topics whose name contains `filter` (ignoring
/// case), plus every always-subscribed topic, which a filter never hides.
[[nodiscard]] inline TopicList visibleTopics(
    const std::map<std::string, std::string>& discovered, const std::string& filter) {
  const std::string needle = PJ::sdk::lowerAscii(filter);
  TopicList out;
  for (const auto& [name, type] : discovered) {
    if (needle.empty() || isAlwaysSubscribed(type) || PJ::sdk::lowerAscii(name).find(needle) != std::string::npos) {
      out.emplace_back(name, type);
    }
  }
  return out;
}

/// The user selection after the table reported `reported`. The host reports
/// only the selectable rows the filter shows, so a selection the filter hides
/// is kept; always-subscribed topics are implied, never stored.
[[nodiscard]] inline TopicList mergeReportedSelection(
    const TopicList& selected, const std::vector<std::string>& reported, const TopicList& visible) {
  const auto find_visible = [&](const std::string& name) {
    return std::find_if(visible.begin(), visible.end(), [&](const auto& v) { return v.first == name; });
  };
  TopicList next;
  for (const auto& entry : selected) {
    if (find_visible(entry.first) == visible.end()) {
      next.push_back(entry);
    }
  }
  for (const auto& name : reported) {
    if (const auto it = find_visible(name); it != visible.end() && !isAlwaysSubscribed(it->second)) {
      next.push_back(*it);
    }
  }
  return next;
}

/// Select All: adds every visible topic that is not always subscribed (those are selected
/// already); a selection the filter hides is kept.
[[nodiscard]] inline TopicList selectAllVisible(TopicList selected, const TopicList& visible) {
  for (const auto& entry : visible) {
    const bool present =
        std::any_of(selected.begin(), selected.end(), [&](const auto& s) { return s.first == entry.first; });
    if (!present && !isAlwaysSubscribed(entry.second)) {
      selected.push_back(entry);
    }
  }
  return selected;
}

/// Fills the topic table `table`: the visible rows, the selection, and the
/// always-subscribed rows shown selected, disabled and tooltipped.
inline void topicTableToWidgets(
    PJ::WidgetData& wd, std::string_view table, const TopicList& visible, const TopicList& selected) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> shown;
  std::vector<int> locked;
  for (const auto& [name, type] : selected) {
    shown.push_back(name);
  }
  for (const auto& [name, type] : visible) {
    if (isAlwaysSubscribed(type)) {
      const int row = static_cast<int>(rows.size());
      locked.push_back(row);
      wd.setCellTooltip(table, row, 0, PJ::AlwaysIncluded::kTooltip);
      shown.push_back(name);
    }
    rows.push_back({name, type});
  }
  wd.setTableHeaders(table, {"Topic", "Datatype"});
  wd.setTableRows(table, rows);
  wd.setDisabledRows(table, locked);
  // Restore selection by topic name: the host matches rows by first-column
  // text, which is sort-agnostic, so it survives a user sort of the table.
  wd.setSelectedItems(table, shown);
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
