#pragma once

#include <algorithm>
#include <map>
#include <pj_always_included/always_included.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <set>
#include <string>
#include <vector>

#include "foxglove_protocol.hpp"

namespace PJ::FoxgloveProtocol {

/// "Latest wins" slot for the host's desired active-topic set (set_active_topics, entered on the host GUI thread).
/// Not a command queue: every write is a full replacement, so only the most recent matters. The poll thread drains it
/// with take(), which hands over what is pending and resets the slot to nullopt.
using DesiredTopicsSlot = PJ::sdk::LatestValueSlot<std::set<std::string>>;

struct SubscriptionDiff {
  std::vector<uint32_t> to_unsubscribe;            ///< subscription ids to drop.
  std::vector<uint64_t> to_subscribe_channel_ids;  ///< advertised channel ids to newly subscribe.
};

/// Pure diff of live subscriptions against the desired active-topic set (declarative and complete).
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

/// A transform type (see common/always_included): its dialog row is locked and the advertise filter never hides it.
[[nodiscard]] inline bool isTransform(const ChannelInfo& ch) {
  return PJ::AlwaysIncluded::matches(ch.schema_name, ch.encoding);
}

/// The dialog's saved channel selection acts in demand mode as an allow-list; an empty selection means no filter.
/// A transform channel always passes.
[[nodiscard]] inline bool passesAdvertiseFilter(const ChannelInfo& ch, const std::vector<ChannelInfo>& selection) {
  return isTransform(ch) ||
         PJ::sdk::passesSelectionFilter(
             ch.topic, selection, [](const ChannelInfo& sel) -> const std::string& { return sel.topic; });
}

/// Supported channels restricted by the same filter buildAvailableTopics applies. Advertise and subscribe stay
/// symmetric: a topic the filter hides is never subscribed, even if a stale desired set names it (the filter is
/// explicit user configuration and outranks a leftover layout reference).
[[nodiscard]] inline std::map<uint64_t, std::string> filteredAdvertisedTopics(
    const std::map<uint64_t, ChannelInfo>& advertised_channels, const std::vector<ChannelInfo>& selection) {
  std::map<uint64_t, std::string> out;
  for (const auto& [channel_id, ch] : advertised_channels) {
    if (classifyChannel(ch).supported && passesAdvertiseFilter(ch, selection)) {
      out[channel_id] = ch.topic;
    }
  }
  return out;
}

}  // namespace PJ::FoxgloveProtocol
