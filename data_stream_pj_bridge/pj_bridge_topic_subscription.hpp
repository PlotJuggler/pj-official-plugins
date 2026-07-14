#pragma once

/// @file pj_bridge_topic_subscription.hpp
/// @brief WebSocket-free control-plane logic for demand-driven per-topic
///  subscription (pj.topic_subscription.v1) in the PlotJuggler Bridge streamer,
///  factored out of pj_bridge_source.cpp so it is unit-testable WITHOUT a live
///  ixwebsocket connection or host — the same seam as
///  data_stream_foxglove_bridge/foxglove_topic_subscription.hpp and
///  data_stream_ros2/ros2_topic_subscription.hpp.
///
///  Twin of those two control planes, adapted to the PJ Bridge wire model:
///  subscriptions are keyed directly by topic NAME (the server's `subscribe` /
///  `unsubscribe` commands take topic names — there is no Foxglove channel-id
///  indirection), the "advertised" universe is the server's get_topics /
///  topics_changed catalog, and `subscribe` is ADDITIVE (a subscribe for one
///  topic never disturbs the others), so a reconcile emits at most one additive
///  `subscribe` and one `unsubscribe` per pass.

#include <algorithm>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pj_bridge_protocol.hpp"

namespace PJ::BridgeProtocol {

/// Mutex-protected "latest wins" slot for the host's desired active-topic set
/// (the payload of pj.topic_subscription.v1's set_active_topics). Twin of
/// PJ::FoxgloveProtocol::DesiredTopicsSlot — identical semantics, duplicated
/// here so data_stream_pj_bridge does not depend on the other streamers.
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

/// Mutex-protected FIFO for the socket-thread → poll-thread handoff of inbound
/// TEXT frames (subscribe responses, topics_changed, get_topics responses).
///
/// THREADING CONTRACT — this is the fix for the demand-mode data race: parser
/// bindings (`bindings_`) are created while decoding a subscribe response and
/// read while draining binary data frames. Both must happen on the SAME (poll)
/// thread. The ixwebsocket callback runs on the socket thread, so it may only
/// ENQUEUE text frames here; the poll thread drains them in arrival order and
/// does all binding work. push() is socket-thread; drain() is poll-thread.
/// FIFO order is preserved so a topics_changed that supersedes an earlier one
/// is applied last.
class TextFrameQueue {
 public:
  /// [socket thread] Enqueue one inbound text frame.
  void push(std::string frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(frame));
  }

  /// [poll thread] Atomically swap out the pending frames for processing,
  /// leaving the shared queue empty. Returned frames are in arrival order.
  [[nodiscard]] std::queue<std::string> drain() {
    std::queue<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(out, queue_);
    return out;
  }

 private:
  std::mutex mutex_;
  std::queue<std::string> queue_;
};

/// Result of computeSubscriptionDiff: which topics to newly subscribe and which
/// to drop, to reconcile the source's current subscriptions with a desired
/// active-topic set. Each list maps to ONE wire command (`subscribe` is
/// additive; `unsubscribe` takes a name list), so an empty list means "send
/// nothing".
struct SubscriptionDiff {
  std::vector<std::string> to_subscribe;    ///< topic names for one additive `subscribe`.
  std::vector<std::string> to_unsubscribe;  ///< topic names for one `unsubscribe`.
};

/// Diff the source's applied subscriptions against a desired active-topic set —
/// declarative and complete, exactly as delivered by pj.topic_subscription.v1.
/// Pure function (no I/O, no side effects): the demand-subscription apply logic
/// in PjBridgeSource::onPoll is unit-testable through this without a live
/// WebSocket.
///
/// The subscribe target is `desired ∩ advertised`: a desired topic the server
/// has not (yet) advertised cannot be subscribed — the caller keeps it in
/// `desired` and retries once/if it appears via topics_changed. A per-topic
/// `subscribe` failure is handled by the caller removing that topic from
/// `applied` (the server forgets a failed subscribe — no sticky retry), so it
/// re-appears in `to_subscribe` on the next reconcile.
///
/// @param applied     topic names with a live subscription right now.
/// @param advertised  topic names the server currently advertises AND that pass
///        the advertise filter (see filteredAdvertisedTopics).
/// @param desired     the full active-topic set the host wants live right now.
[[nodiscard]] inline SubscriptionDiff computeSubscriptionDiff(
    const std::set<std::string>& applied, const std::set<std::string>& advertised,
    const std::set<std::string>& desired) {
  SubscriptionDiff diff;

  // Drop anything applied that fell out of the target (undesired OR no longer advertised).
  for (const auto& topic : applied) {
    if (desired.count(topic) == 0 || advertised.count(topic) == 0) {
      diff.to_unsubscribe.push_back(topic);
    }
  }

  // Subscribe anything in the target that is not applied yet.
  for (const auto& topic : advertised) {
    if (desired.count(topic) > 0 && applied.count(topic) == 0) {
      diff.to_subscribe.push_back(topic);
    }
  }

  return diff;
}

/// Whether `topic` should be advertised/subscribed under an (optional) filter —
/// the dialog's saved topic selection, repurposed in demand mode as an
/// allow-list rather than a subscribe list. An EMPTY selection means "no
/// filter, advertise everything" (the default when nothing is selected).
/// Mirrors PJ::FoxgloveProtocol::passesAdvertiseFilter.
[[nodiscard]] inline bool passesAdvertiseFilter(const std::string& topic, const std::vector<std::string>& selection) {
  return selection.empty() ||
         std::any_of(selection.begin(), selection.end(), [&](const std::string& s) { return s == topic; });
}

/// The set of topic names computeSubscriptionDiff() reconciles against:
/// server-advertised topics restricted by the SAME filter buildAvailableTopics
/// applies when advertising. Advertise and subscribe stay symmetric — a topic
/// the filter hides from the host is never subscribed either, even if a (stale)
/// desired set names it: the filter is explicit user configuration and outranks
/// a leftover layout reference. Mirrors
/// PJ::FoxgloveProtocol::filteredAdvertisedTopics.
[[nodiscard]] inline std::set<std::string> filteredAdvertisedTopics(
    const std::map<std::string, TopicInfo>& advertised, const std::vector<std::string>& selection) {
  std::set<std::string> out;
  for (const auto& [name, _info] : advertised) {
    if (passesAdvertiseFilter(name, selection)) {
      out.insert(name);
    }
  }
  return out;
}

/// Parse one get_topics / topics_changed.added entry into a TopicInfo.
///
/// Tolerant of the include_schemas graceful-degradation path: a new server
/// returns name + type + encoding + definition, but an OLD server (which
/// ignores include_schemas) and a topic whose schema extraction FAILED both
/// return only name + type — the encoding/schema fields are then left empty,
/// and the topic is still listed (the authoritative schema still arrives later
/// in the subscribe RESPONSE, which every server sends). Returns nullopt when
/// the entry is not an object or carries no name.
///
/// `type` is the ROS 2 message type; the parser binding uses it as `type_name`.
/// An explicit `schema_name`, if present, wins over `type`.
/// json::value() THROWS when the key exists with a non-string type, and these
/// entries come straight off the wire — a malformed field must degrade to
/// "absent", never throw into the poll loop.
[[nodiscard]] inline std::string stringField(const nlohmann::json& entry, const char* key) {
  const auto it = entry.find(key);
  return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

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

/// Names in `applied` whose catalog entry CHANGED between two advertise states
/// (present in both, fields differ). A retyped/re-schemaed topic that is
/// currently subscribed keeps flowing bytes the OLD parser binding would
/// misparse — the caller drops these from applied_/bindings_ so the next
/// reconcile re-subscribes and the fresh subscribe response re-binds with the
/// new schema (the host mints a fresh binding when the signature changed).
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

/// The protocol version this plugin speaks. The ONLY hard compatibility gate:
/// a response carrying a HIGHER version means breaking changes this client
/// cannot interpret, so the stream stops with an upgrade message. Additive
/// server features never bump it — they are feature-detected via ServerInfo.
inline constexpr int kSupportedProtocolVersion = 1;

/// Returns the response's protocol_version when it is above `supported` —
/// the caller turns that into a fatal stream error. Absent or non-integer
/// versions yield nullopt (old/test servers predate the field; tolerance is
/// the compatibility policy for everything except a KNOWN-newer protocol).
[[nodiscard]] inline std::optional<int> unsupportedProtocolVersion(const nlohmann::json& response, int supported) {
  const auto it = response.find("protocol_version");
  if (it == response.end() || !it->is_number_integer()) {
    return std::nullopt;
  }
  const int version = it->get<int>();
  return version > supported ? std::optional<int>(version) : std::nullopt;
}

/// Server identity + capability list from a get_topics response's `server`
/// object. Clients feature-detect by capability NAME and degrade per-feature;
/// `version` is for humans/bug reports, never compared.
struct ServerInfo {
  std::string name;
  std::string version;
  std::set<std::string> capabilities;

  [[nodiscard]] bool hasCapability(const std::string& capability) const {
    return capabilities.count(capability) > 0;
  }
};

/// nullopt when the response has no `server` object (a pre-capability server:
/// treat every capability as absent). Wrong-typed fields degrade to empty.
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

/// Whether a per-topic failure in a subscribe RESPONSE is stale: the response
/// id (echoed by the server) predates the LAST subscribe we sent for that
/// topic, so the failure describes a request that a newer subscribe already
/// superseded — acting on it would erase applied_'s truth for a live
/// subscription (a later pause would then never send the unsubscribe).
/// A response without an id (older/test servers) is never treated as stale.
[[nodiscard]] inline bool failureIsStale(
    const std::map<std::string, std::string>& last_subscribe_id_by_topic, const std::string& topic,
    const std::string& response_id) {
  if (response_id.empty()) {
    return false;
  }
  const auto it = last_subscribe_id_by_topic.find(topic);
  return it != last_subscribe_id_by_topic.end() && it->second != response_id;
}

/// Apply a topics_changed (or initial get_topics) delta to the advertised
/// catalog: upsert each `added` topic (a re-add with new fields replaces the
/// old entry) and erase each `removed` name. Returns true iff the catalog
/// actually changed, so the caller re-advertises to the host and reconciles
/// only on a genuine delta. Pure — the JSON→TopicInfo parsing (via
/// parseTopicEntry) is the caller's, so this stays trivially testable.
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
