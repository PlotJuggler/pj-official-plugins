#pragma once

#include <algorithm>
#include <map>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <set>
#include <string>
#include <vector>

#include "foxglove_protocol.hpp"

namespace PJ::FoxgloveProtocol {

/// Mutex-protected "latest wins" slot for the host's desired active-topic set
/// (the payload of pj.topic_subscription.v1's set_active_topics).
///
/// set_active_topics() is entered on the HOST GUI thread; this is NOT a command
/// queue — every call is a full, declarative replacement of the desired set, so
/// only the MOST RECENT write matters. The poll thread drains it with take(),
/// which atomically hands over whatever is pending and resets the slot to empty
/// (nullopt), so a poll pass that finds nothing new is a cheap no-op.
using DesiredTopicsSlot = PJ::sdk::LatestValueSlot<std::set<std::string>>;

/// Result of computeSubscriptionDiff: which live subscriptions to drop and which
/// advertised channels to newly subscribe, to reconcile the source's current
/// subscriptions with a desired active-topic set.
struct SubscriptionDiff {
  std::vector<uint32_t> to_unsubscribe;            ///< subscription ids to drop.
  std::vector<uint64_t> to_subscribe_channel_ids;  ///< advertised channel ids to newly subscribe.
};

/// Diff the source's live subscriptions against a desired active-topic set —
/// declarative and complete, exactly as delivered by pj.topic_subscription.v1.
/// Pure function (no I/O, no side effects): the demand-subscription apply logic
/// in FoxgloveSource::onPoll is unit-testable through this without a live
/// WebSocket.
///
/// @param current_subscriptions      sub_id -> channel_id, the source's live subscriptions.
/// @param advertised_channel_topics  channel_id -> topic name, restricted to channels the
///        source can actually subscribe to (i.e. classifyChannel(...).supported). A topic
///        with no entry here cannot be subscribed even if desired — the caller is expected
///        to remember it and retry once/if the channel is (re-)advertised.
/// @param desired_topics  the full active-topic set the host wants live right now.
[[nodiscard]] inline SubscriptionDiff computeSubscriptionDiff(
    const std::map<uint32_t, uint64_t>& current_subscriptions,
    const std::map<uint64_t, std::string>& advertised_channel_topics, const std::set<std::string>& desired_topics) {
  SubscriptionDiff diff;

  std::set<uint64_t> subscribed_channel_ids;
  for (const auto& [sub_id, channel_id] : current_subscriptions) {
    subscribed_channel_ids.insert(channel_id);
    const auto it = advertised_channel_topics.find(channel_id);
    const bool keep = it != advertised_channel_topics.end() && desired_topics.count(it->second) > 0;
    if (!keep) {
      diff.to_unsubscribe.push_back(sub_id);
    }
  }

  for (const auto& [channel_id, topic] : advertised_channel_topics) {
    if (desired_topics.count(topic) > 0 && subscribed_channel_ids.count(channel_id) == 0) {
      diff.to_subscribe_channel_ids.push_back(channel_id);
    }
  }

  return diff;
}

/// Whether `topic` should be advertised under an (optional) advertise filter —
/// the dialog's saved channel selection, repurposed in demand mode as an
/// allow-list rather than a subscribe list. An EMPTY selection means "no
/// filter, advertise everything" (the default when nothing is selected).
[[nodiscard]] inline bool passesAdvertiseFilter(const std::string& topic, const std::vector<ChannelInfo>& selection) {
  return PJ::sdk::passesSelectionFilter(
      topic, selection, [](const ChannelInfo& ch) -> const std::string& { return ch.topic; });
}

/// The channel_id -> topic map computeSubscriptionDiff() reconciles against:
/// supported channels only, restricted by the SAME advertise filter
/// buildAvailableTopics applies. Advertise and subscribe stay symmetric — a
/// topic the filter hides from the host is never subscribed either, even if a
/// (stale) desired set names it: the filter is explicit user configuration and
/// outranks a leftover layout reference.
[[nodiscard]] inline std::map<uint64_t, std::string> filteredAdvertisedTopics(
    const std::map<uint64_t, ChannelInfo>& advertised_channels, const std::vector<ChannelInfo>& selection) {
  std::map<uint64_t, std::string> out;
  for (const auto& [channel_id, ch] : advertised_channels) {
    if (classifyChannel(ch).supported && passesAdvertiseFilter(ch.topic, selection)) {
      out[channel_id] = ch.topic;
    }
  }
  return out;
}

}  // namespace PJ::FoxgloveProtocol
