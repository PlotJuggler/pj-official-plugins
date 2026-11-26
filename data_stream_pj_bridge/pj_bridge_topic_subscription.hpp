#pragma once

/// @file pj_bridge_topic_subscription.hpp
/// Socket-free control plane for demand-driven per-topic subscription (pj.topic_subscription.v1), split out of
/// pj_bridge_source.cpp so it is unit-testable without a connection. Twin of foxglove_topic_subscription.hpp and
/// ros2_topic_subscription.hpp, adapted to this wire model: subscriptions are keyed by topic NAME, the advertised
/// universe is the server's get_topics / topics_changed catalog, and `subscribe` is additive (one topic never
/// disturbs the others), so a reconcile emits at most one `subscribe` and one `unsubscribe`.

#include <algorithm>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_always_included/always_included.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pj_bridge_protocol.hpp"

namespace PJ::BridgeProtocol {

/// "Latest wins" slot for the host's desired active-topic set (set_active_topics, entered on the host GUI thread).
/// Not a command queue: every write is a full replacement, so only the most recent matters. The poll thread drains it
/// with take(), which hands over what is pending and resets the slot to nullopt.
using DesiredTopicsSlot = PJ::sdk::LatestValueSlot<std::set<std::string>>;

/// Each list maps to one wire command (`subscribe` is additive; `unsubscribe` takes a name list); empty means send
/// nothing.
struct SubscriptionDiff {
  std::vector<std::string> to_subscribe;    ///< topic names for one additive `subscribe`.
  std::vector<std::string> to_unsubscribe;  ///< topic names for one `unsubscribe`.
};

/// Pure diff of applied subscriptions against the desired active-topic set (declarative and complete).
///
/// The subscribe target is `desired ∩ advertised`: a desired topic the server has not (yet) advertised stays in
/// `desired` and is retried when it appears via topics_changed. A per-topic `subscribe` failure is handled by the
/// caller removing the topic from `applied` (the server forgets a failed subscribe, no sticky retry), so it re-appears
/// in `to_subscribe` on the next reconcile.
///
/// @param advertised  topics the server advertises that also pass the advertise filter (see filteredAdvertisedTopics).
[[nodiscard]] inline SubscriptionDiff computeSubscriptionDiff(
    const std::set<std::string>& applied, const std::set<std::string>& advertised,
    const std::set<std::string>& desired) {
  SubscriptionDiff diff;

  for (const auto& topic : applied) {
    if (desired.count(topic) == 0 || advertised.count(topic) == 0) {
      diff.to_unsubscribe.push_back(topic);
    }
  }

  for (const auto& topic : advertised) {
    if (desired.count(topic) > 0 && applied.count(topic) == 0) {
      diff.to_subscribe.push_back(topic);
    }
  }

  return diff;
}

/// The dialog's saved topic selection acts in demand mode as an allow-list; an empty selection means no filter.
/// A transform type (PJ::AlwaysIncluded::matchesRos2 on the topic's ROS 2 type) always passes.
[[nodiscard]] inline bool passesAdvertiseFilter(const TopicInfo& topic, const std::vector<std::string>& selection) {
  return PJ::AlwaysIncluded::matchesRos2(topic.schema_name) ||
         PJ::sdk::passesSelectionFilter(
             topic.name, selection, [](const std::string& value) -> const std::string& { return value; });
}

/// Server-advertised topics restricted by the same filter buildAvailableTopics applies. Advertise and subscribe stay
/// symmetric: a topic the filter hides is never subscribed, even if a stale desired set names it (the filter is
/// explicit user configuration and outranks a leftover layout reference).
[[nodiscard]] inline std::set<std::string> filteredAdvertisedTopics(
    const std::map<std::string, TopicInfo>& advertised, const std::vector<std::string>& selection) {
  std::set<std::string> out;
  for (const auto& [name, info] : advertised) {
    if (passesAdvertiseFilter(info, selection)) {
      out.insert(name);
    }
  }
  return out;
}

/// json::value() throws when the key exists with a non-string type; these entries come straight off the wire, so a
/// malformed field must degrade to "absent", never throw into the poll loop.
[[nodiscard]] inline std::string stringField(const nlohmann::json& entry, const char* key) {
  const auto it = entry.find(key);
  return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// Parse one get_topics / topics_changed.added entry. An old server (ignores include_schemas) or a topic whose schema
/// extraction failed returns only name + type: encoding/schema stay empty and the topic is still listed (the
/// authoritative schema arrives in the subscribe RESPONSE). `type` is the ROS 2 message type, used as the parser
/// binding's `type_name`; an explicit `schema_name` wins. Returns nullopt when the entry is not an object or has no
/// name.
[[nodiscard]] inline std::optional<TopicInfo> parseTopicEntry(const nlohmann::json& entry) {
  if (!entry.is_object()) {
    return std::nullopt;
  }
  std::string name = stringField(entry, "name");
  if (name.empty()) {
    return std::nullopt;
  }
  TopicInfo info;
  info.name = std::move(name);
  const std::string schema_name = stringField(entry, "schema_name");
  info.schema_name = schema_name.empty() ? stringField(entry, "type") : schema_name;
  info.encoding = stringField(entry, "encoding");
  info.schema = stringField(entry, "definition");
  return info;
}

/// Names in `applied` whose catalog entry changed between two advertise states. A retyped subscribed topic keeps
/// flowing bytes the old parser binding would misparse, so the caller drops these from applied_/bindings_; the next
/// reconcile re-subscribes and the fresh response re-binds with the new schema.
[[nodiscard]] inline std::vector<std::string> retypedTopics(
    const std::map<std::string, TopicInfo>& before, const std::map<std::string, TopicInfo>& after,
    const std::set<std::string>& applied) {
  std::vector<std::string> retyped;
  for (const auto& name : applied) {
    const auto old_it = before.find(name);
    const auto new_it = after.find(name);
    if (old_it != before.end() && new_it != after.end() && !(old_it->second == new_it->second)) {
      retyped.push_back(name);
    }
  }
  return retyped;
}

/// The only hard compatibility gate: a response with a higher version means breaking changes, so the stream stops with
/// an upgrade message. Additive server features never bump it; they are feature-detected via ServerInfo.
inline constexpr int kSupportedProtocolVersion = 1;

/// The response's protocol_version when above `supported` (the caller makes it a fatal stream error). Absent or
/// non-integer versions yield nullopt: old/test servers predate the field. Compared without narrowing, so a version
/// too large for an int is still newer.
[[nodiscard]] inline std::optional<uint64_t> unsupportedProtocolVersion(const nlohmann::json& response, int supported) {
  const auto it = response.find("protocol_version");
  if (it == response.end() || !it->is_number_integer() || (!it->is_number_unsigned() && it->get<int64_t>() < 0)) {
    return std::nullopt;  // absent, wrong-typed, or negative (older than any version)
  }
  const auto version = it->get<uint64_t>();
  return version > static_cast<uint64_t>(supported) ? std::optional<uint64_t>(version) : std::nullopt;
}

/// From a get_topics response's `server` object. Clients feature-detect by capability name; `version` is for humans,
/// never compared.
struct ServerInfo {
  std::string name;
  std::string version;
  std::set<std::string> capabilities;

  [[nodiscard]] bool hasCapability(const std::string& capability) const {
    return capabilities.count(capability) > 0;
  }
};

/// nullopt when there is no `server` object (pre-capability server: every capability is absent). Wrong-typed fields
/// degrade to empty.
[[nodiscard]] inline std::optional<ServerInfo> parseServerInfo(const nlohmann::json& response) {
  const auto it = response.find("server");
  if (it == response.end() || !it->is_object()) {
    return std::nullopt;
  }
  ServerInfo info;
  info.name = stringField(*it, "name");
  info.version = stringField(*it, "version");
  const auto caps = it->find("capabilities");
  if (caps != it->end() && caps->is_array()) {
    for (const auto& capability : *caps) {
      if (capability.is_string()) {
        info.capabilities.insert(capability.get<std::string>());
      }
    }
  }
  return info;
}

/// A per-topic failure is stale when the echoed response id predates the last subscribe sent for that topic: acting on
/// it would erase applied_ for a live subscription, so a later pause would never send the unsubscribe. A response
/// without an id (older/test servers) is never stale.
[[nodiscard]] inline bool failureIsStale(
    const std::map<std::string, std::string>& last_subscribe_id_by_topic, const std::string& topic,
    const std::string& response_id) {
  if (response_id.empty()) {
    return false;
  }
  const auto it = last_subscribe_id_by_topic.find(topic);
  return it != last_subscribe_id_by_topic.end() && it->second != response_id;
}

/// Upserts `added` and erases `removed`. Returns true iff the catalog actually changed, so the caller re-advertises
/// and reconciles only on a real delta.
[[nodiscard]] inline bool applyAdvertiseDelta(
    std::map<std::string, TopicInfo>& advertised, const std::vector<TopicInfo>& added,
    const std::vector<std::string>& removed) {
  bool changed = false;
  for (const auto& info : added) {
    auto it = advertised.find(info.name);
    if (it == advertised.end()) {
      advertised.emplace(info.name, info);
      changed = true;
    } else if (!(it->second == info)) {
      it->second = info;
      changed = true;
    }
  }
  for (const auto& name : removed) {
    if (advertised.erase(name) > 0) {
      changed = true;
    }
  }
  return changed;
}

}  // namespace PJ::BridgeProtocol
