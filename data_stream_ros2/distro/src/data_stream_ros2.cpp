/**
 * @file data_stream_ros2.cpp
 * @brief ROS 2 topic subscriber — per-distro implementation.
 *
 * Subscribes to a user-selected list of ROS 2 topics, collects raw CDR bytes
 * via `GenericSubscription`, and hands them off to the host parser registry
 * (delegated ingest). The actual decoding lives in `parser_ros`, which we
 * reach via `runtimeHost().ensureParserBinding({encoding="ros2msg", ...})`.
 *
 * Threading: a `MultiThreadedExecutor` runs in `spinner_`, callbacks enqueue
 * messages into a mutex-protected queue, and `onPoll()` drains the queue on
 * the host's polling thread (per the SDK contract — host write methods may
 * only be called from `onPoll()`).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <queue>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialized_message.hpp>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../ros2_topic_subscription.hpp"
#include "ros2_dialog.hpp"
#include "ros2_manifest.hpp"
#include "ros2_qos_adapter.hpp"
#include "ros2_schema_builder.hpp"

namespace {

struct PendingMessage {
  std::string topic;
  // Hold the rclcpp::SerializedMessage shared_ptr so its rcl buffer survives
  // any deferred FetchMessageData pull the host issues. The same shared_ptr
  // is forwarded as the PayloadView anchor at push time.
  std::shared_ptr<rclcpp::SerializedMessage> msg;
  int64_t timestamp_ns;
};

class Ros2StreamSource : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog | PJ::kCapabilityPerTopicPause;
  }

  /// pj.topic_subscription.v1 (host -> plugin): the host pushes the full active-
  /// topic set here (see setActiveTopicsThunk). Every other extension id is
  /// unknown -> nullptr, the default DataSourcePluginBase::pluginExtension()
  /// behavior for a host that doesn't ask for this one.
  const void* pluginExtension(std::string_view id) override {
    if (id == PJ_TOPIC_SUBSCRIPTION_EXTENSION_V1) {
      static const PJ_topic_subscription_v1_t ext{sizeof(PJ_topic_subscription_v1_t), &setActiveTopicsThunk};
      return &ext;
    }
    return nullptr;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    // Forward unconditionally (even on an empty config): dialog_.loadConfig()
    // both restores any saved selection and starts topic discovery, which must
    // run on a fresh open too. Mirrors data_stream_mqtt's loadConfig.
    (void)dialog_.loadConfig(config_json);
    // Extract parser config (e.g. "use_embedded_timestamp") persisted by the
    // host under "_parser_config" so it reaches ensureParserBinding.
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded() && cfg.contains("_parser_config")) {
      parser_config_override_ = cfg["_parser_config"].get<std::string>();
    } else {
      parser_config_override_.clear();
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    selected_topics_.clear();
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      for (const auto& entry : cfg["selected_topics"]) {
        if (!entry.is_object()) {
          continue;
        }
        std::string name = entry.value("name", std::string{});
        std::string type = entry.value("type", std::string{});
        if (!name.empty() && !type.empty()) {
          selected_topics_.emplace_back(std::move(name), std::move(type));
        }
      }
    }
    // Selection is now OPTIONAL: on a host that supports demand subscriptions
    // (decided below by the first notify_available_topics attempt), an empty
    // selection just means "advertise everything" — the host asks for exactly
    // what it wants via set_active_topics. A legacy host with an empty
    // selection is handled after demand_mode_ is decided, below (warns instead
    // of failing outright, matching data_stream_foxglove_bridge).

    // Stash the parser_config sub-object as a string for every ensureBinding
    // call. parser_ros::loadConfig accepts unknown keys silently, so unknown
    // future options pass through harmlessly.
    parser_config_json_.clear();
    if (cfg.contains("parser_config") && cfg["parser_config"].is_object()) {
      parser_config_json_ = cfg["parser_config"].dump();
    }

    try {
      context_ = std::make_shared<rclcpp::Context>();
      context_->init(0, nullptr);

      rclcpp::NodeOptions node_opts;
      node_opts.context(context_);
      node_ = std::make_shared<rclcpp::Node>("plotjuggler_ros2_subscriber", node_opts);

      rclcpp::ExecutorOptions exec_opts;
      exec_opts.context = context_;
      executor_ = std::make_unique<rclcpp::executors::MultiThreadedExecutor>(exec_opts, 2);
      executor_->add_node(node_);

      // IMPORTANT ORDERING: the very first notify_available_topics call (just
      // below) needs discovery data, but this brand-new Context/Node has not
      // seen any DDS discovery traffic yet — querying it right now would very
      // likely come back empty. Fall back to the dialog's saved selection
      // (name/type pairs already known from a previous session) ONLY when the
      // real scan is empty, so the first advertise carries at least the
      // previously-known topics instead of an empty catalog while the host
      // waits for the next ~1s discovery refresh (see
      // refreshDiscoveryAndAdvertise). An empty first advertise is still a
      // valid demand-mode probe — it only decides demand_mode_ below.
      discovered_topics_ = scanDiscovery();
      last_discovery_refresh_ = std::chrono::steady_clock::now();
      if (discovered_topics_.empty()) {
        for (const auto& [name, type] : selected_topics_) {
          discovered_topics_[name] = type;
        }
      }

      // Demand mode is decided ONCE per start, by the very first advertise
      // attempt: a host that lacks the notify_available_topics tail slot (old
      // PJ4) returns an explicit error, so a new plugin detects it and
      // degrades to the legacy selection-driven subscribe path below,
      // byte-for-byte. A host that accepts it is re-notified on every
      // discovery delta (see refreshDiscoveryAndAdvertise) and drives
      // subscriptions solely through pj.topic_subscription.v1 (see onPoll /
      // applyDesiredTopics) — demand mode never auto-subscribes here.
      demand_mode_ = static_cast<bool>(refreshAdvertisedTopics());

      if (!demand_mode_) {
        if (selected_topics_.empty()) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "No ROS 2 topics selected — connected to a legacy PlotJuggler host, which cannot "
              "advertise topics on demand. Nothing will stream until the connection dialog is "
              "reopened and topics are selected.");
        }
        for (const auto& [topic, type] : selected_topics_) {
          subscribeTopic(topic, type, std::chrono::milliseconds(2000));
        }
      }

      running_ = true;
      spinner_ = std::thread([this] {
        while (running_) {
          executor_->spin_once(std::chrono::milliseconds(50));
        }
      });
    } catch (const std::exception& e) {
      teardown();
      return PJ::unexpected(std::string("ROS 2 setup failed: ") + e.what());
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Demand mode: refresh the streamer's own ROS-graph discovery (throttled to
    // ~1s) and re-advertise + reconcile on any change, then drain the host's
    // latest desired active-topic set (if any) and reconcile again — desired-
    // then-discovered and discovered-then-desired both converge on
    // reconcileSubscriptions() instead of two separate code paths. Both calls
    // are poll-thread-only and non-blocking (see their doc comments).
    if (demand_mode_) {
      refreshDiscoveryAndAdvertise();
      applyDesiredTopics();
    }

    std::queue<PendingMessage> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, message_queue_);
    }

    while (!batch.empty()) {
      auto& msg = batch.front();
      auto* binding = ensureBinding(msg.topic);
      if (binding != nullptr) {
        // Zero-copy push: the FetchMessageData closure captures the
        // SerializedMessage shared_ptr (which owns the rcl buffer) and hands
        // the host a PayloadView pointing at that buffer with the shared_ptr
        // as the BufferAnchor. The host applies ObjectIngestPolicy to decide
        // whether to invoke the closure now (eager), on consumer pull
        // (lazy), or never. The buffer survives any pending pull because
        // every captured shared_ptr keeps the SerializedMessage alive.
        auto status = runtimeHost().pushMessage(
            *binding, PJ::Timestamp{msg.timestamp_ns}, [keeper = msg.msg]() -> PJ::sdk::PayloadView {
              const auto& rcl = keeper->get_rcl_serialized_message();
              return PJ::sdk::PayloadView{
                  PJ::Span<const uint8_t>{rcl.buffer, rcl.buffer_length},
                  PJ::sdk::BufferAnchor{keeper},
              };
            });
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "Failed to push ROS 2 message on " + msg.topic + ": " + status.error());
        }
      }
      batch.pop();
    }
    return PJ::okStatus();
  }

  void onStop() override {
    teardown();
  }

 private:
  void enqueueMessage(const std::string& topic, std::shared_ptr<rclcpp::SerializedMessage> msg) {
    if (!msg) {
      return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    PendingMessage pending{
        .topic = topic,
        .msg = std::move(msg),
        .timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
    };

    std::lock_guard<std::mutex> lock(queue_mutex_);
    message_queue_.push(std::move(pending));
  }

  // Resolve the ROS 2 type name for `topic` from subscription_types_ — the
  // single source of truth for whatever type actually created its live
  // GenericSubscription (see subscribeTopic). Deliberately NOT re-derived
  // from selected_topics_/discovered_topics_: either mode could in principle
  // hold a different type string for the same topic name (e.g. a stale
  // selection saved before a topic was re-typed), and ensureBinding's parser
  // schema MUST match whatever bytes the subscription is actually decoding,
  // not a plausible-but-unverified lookup. A topic only reaches here once it
  // has produced a message, which requires it to already be subscribed.
  std::string resolveTypeForTopic(const std::string& topic) const {
    auto it = subscription_types_.find(topic);
    return it != subscription_types_.end() ? it->second : std::string{};
  }

  PJ::ParserBindingHandle* ensureBinding(const std::string& topic) {
    auto it = binding_cache_.find(topic);
    if (it != binding_cache_.end()) {
      return &it->second;
    }

    const std::string type_name = resolveTypeForTopic(topic);
    if (type_name.empty()) {
      return nullptr;
    }

    const std::string* schema = ensureSchemaForType(type_name);
    if (schema == nullptr) {
      return nullptr;  // already warned once by ensureSchemaForType
    }

    nlohmann::json parser_config = nlohmann::json::object();
    const std::string& base_parser_config =
        !parser_config_override_.empty() ? parser_config_override_ : parser_config_json_;
    if (!base_parser_config.empty()) {
      auto parsed_config = nlohmann::json::parse(base_parser_config, nullptr, false);
      if (parsed_config.is_object()) {
        parser_config = std::move(parsed_config);
      }
    }
    parser_config["schema_encoding"] = "ros2msg";
    const std::string parser_config_str = parser_config.dump();

    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = topic,
        .parser_encoding = "ros2msg",
        .type_name = type_name,
        .schema = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema->data()), schema->size()),
        .parser_config_json = parser_config_str,
    });
    if (!binding) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Parser binding failed for " + topic + ": " + binding.error());
      return nullptr;
    }
    auto [iter, _] = binding_cache_.emplace(topic, *binding);
    return &iter->second;
  }

  // Resolve (and cache) the synthesized .msg schema text for `type_name`.
  // Returns nullptr (after a one-time warning) if introspection failed to
  // resolve the type. Shared by buildAvailableTopics() (advertise) and
  // ensureBinding() (subscribe, either mode) — both need the exact same bytes
  // for the exact same type, so one cache serves both and a persistently
  // unresolvable type only ever warns once instead of on every message.
  const std::string* ensureSchemaForType(const std::string& type_name) {
    auto it = schema_by_type_.find(type_name);
    if (it != schema_by_type_.end()) {
      return &it->second;
    }
    try {
      auto [inserted, ok] = schema_by_type_.emplace(type_name, ros2_streamer::buildRos2Schema(type_name));
      (void)ok;
      return &inserted->second;
    } catch (const std::exception& e) {
      if (warned_schema_types_.insert(type_name).second) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "Failed to build schema for type " + type_name + ": " + e.what() +
                                                      " (topics of this type will not be advertised or bound)");
      }
      return nullptr;
    }
  }

  // Create and register a generic subscription for (topic, type), adapting
  // QoS to whatever publishers are visible within `qos_wait`. Shared by the
  // legacy subscribe-selected-at-onStart path (qos_wait = the original 2s
  // discovery window — the Context is brand new there and DDS discovery may
  // not have propagated yet) and the demand-mode reconcile path on the poll
  // thread (qos_wait = 0, i.e. sample once and don't block — see
  // reconcileSubscriptions).
  void subscribeTopic(const std::string& topic, const std::string& type, std::chrono::milliseconds qos_wait) {
    const auto qos = ros2_streamer::adaptQosWaitingForPublishers(*node_, topic, qos_wait);
    if (node_->count_publishers(topic) == 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "No publishers visible for " + topic + " within discovery timeout — using default QoS");
    }

    auto callback = [this, topic](std::shared_ptr<rclcpp::SerializedMessage> msg) {
      enqueueMessage(topic, std::move(msg));
    };

    auto subscription = node_->create_generic_subscription(topic, type, qos, callback);
    subscriptions_.emplace(topic, std::move(subscription));
    subscription_types_[topic] = type;
  }

  // [poll thread] Scan the ROS graph once via get_topic_names_and_types(),
  // excluding multi-type topics (warned once each — see splitTopicTypes).
  std::map<std::string, std::string> scanDiscovery() {
    auto split = ros2_streamer::splitTopicTypes(node_->get_topic_names_and_types());
    for (const auto& topic : split.multi_type) {
      if (warned_multi_type_topics_.insert(topic).second) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "Topic " + topic + " is published under more than one type — skipping (unsupported).");
      }
    }
    return std::move(split.single_type);
  }

  // Build the AvailableTopic list for notify_available_topics from
  // discovered_topics_: restricted by the dialog's saved selection
  // (selected_topics_), which is dual-purpose (see ros2_dialog.hpp's OK-gate
  // comment): on a legacy host it is the subscribe list (the loop in
  // onStart); here, in demand mode, it acts as an OPTIONAL advertise filter —
  // an empty selection means "advertise everything". Schema bytes alias
  // schema_by_type_'s cached strings, which outlive the
  // synchronously-consumed notify_available_topics call below.
  std::vector<PJ::AvailableTopic> buildAvailableTopics() {
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(discovered_topics_.size());
    for (const auto& [topic, type] : discovered_topics_) {
      if (!ros2_streamer::passesAdvertiseFilter(topic, selected_topics_)) {
        continue;
      }
      const std::string* schema = ensureSchemaForType(type);
      if (schema == nullptr) {
        continue;  // schema build failed — already warned once by ensureSchemaForType
      }
      topics.push_back(
          PJ::AvailableTopic{
              topic, "ros2msg", type,
              PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema->data()), schema->size())});
    }
    return topics;
  }

  // Advertise the current (filtered) full topic set to the host. The FIRST
  // call, from onStart(), also decides demand_mode_ via the returned status.
  // Later calls (from refreshDiscoveryAndAdvertise, after a discovery delta)
  // are only made when demand_mode_ is already true — no point re-probing a
  // host that has already been found to lack the slot.
  PJ::Status refreshAdvertisedTopics() {
    const auto topics = buildAvailableTopics();
    return runtimeHost().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  }

  // Demand-mode wrapper around refreshAdvertisedTopics() for the steady-state
  // calls (discovery deltas), where a failure is only worth a warning —
  // unlike the first call in onStart(), whose status DECIDES demand_mode_.
  void refreshAdvertisedTopicsAndWarn() {
    if (auto status = refreshAdvertisedTopics(); !status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Failed to advertise topics: " + status.error());
    }
  }

  // [poll thread] Throttled (~1s) ROS-graph discovery scan, mirroring the
  // dialog's own refreshFromNode() cadence (ros2_dialog.hpp) but run
  // independently by the streamer's own node (the dialog's node is torn down
  // on accept/reject, long before the streamer starts). Re-advertises the
  // full current set via notify_available_topics whenever the discovered set
  // (or a topic's type) changes, and re-runs reconcileSubscriptions() so a
  // topic that just became discoverable — but was already desired — gets
  // subscribed without waiting for another set_active_topics call.
  void refreshDiscoveryAndAdvertise() {
    if (!node_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_discovery_refresh_ < std::chrono::seconds(1)) {
      return;
    }
    last_discovery_refresh_ = now;

    std::map<std::string, std::string> topics;
    try {
      topics = scanDiscovery();
    } catch (const std::exception& e) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, std::string("ROS 2 discovery scan failed: ") + e.what());
      return;
    }

    if (topics == discovered_topics_) {
      return;  // nothing changed — skip the re-advertise + reconcile
    }
    discovered_topics_ = std::move(topics);

    refreshAdvertisedTopicsAndWarn();
    reconcileSubscriptions();
  }

  // [poll thread] The single reconciliation point for demand mode: diff the
  // live subscriptions against last_applied_desired_ over the
  // currently-discovered (and filter-passing) topics, unsubscribing what fell
  // out and subscribing what is newly wanted and discoverable. Called from
  // applyDesiredTopics (a new desired set landed) AND from
  // refreshDiscoveryAndAdvertise (after a discovery delta) — so
  // desired-then-discovered and discovered-then-desired converge on this one
  // code path instead of two.
  void reconcileSubscriptions() {
    // Filter-aware: a topic the advertise filter hides from the host must not
    // be subscribed either (see filteredDiscoveredTopics) — and a live
    // subscription to a now-filtered topic is dropped by the same diff.
    const auto subscribable = ros2_streamer::filteredDiscoveredTopics(discovered_topics_, selected_topics_);

    std::set<std::string> current;
    for (const auto& [topic, subscription] : subscriptions_) {
      (void)subscription;
      current.insert(topic);
    }

    const auto diff = ros2_streamer::computeRos2SubscriptionDiff(current, subscribable, last_applied_desired_);

    for (const auto& topic : diff.to_unsubscribe) {
      // map erase -> shared_ptr drop -> DDS unsubscribe. binding_cache_'s
      // entry (if any) is intentionally LEFT in place: parser bindings are
      // host-side state, not a live subscription — an idle cached binding
      // costs nothing, and re-subscribing the same topic later reuses it
      // instead of re-binding.
      subscriptions_.erase(topic);
      subscription_types_.erase(topic);
    }

    for (const auto& topic : diff.to_subscribe) {
      const auto it = subscribable.find(topic);
      if (it == subscribable.end()) {
        continue;  // should not happen: diff only proposes subscribable topics
      }
      try {
        // onPoll MUST NOT BLOCK — the topic just came out of a completed
        // discovery scan, so its publisher's QoS is normally already
        // resolvable without waiting; qos_wait=0 makes
        // adaptQosWaitingForPublishers sample once and return immediately
        // instead of polling for up to 2s like onStart's initial subscribe.
        subscribeTopic(topic, it->second, std::chrono::milliseconds(0));
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "Failed to subscribe to " + topic + ": " + e.what());
      }
    }
  }

  // [poll thread] Apply the host's latest desired active-topic set, if it
  // wrote one since the last poll (latest-wins slot — see
  // setActiveTopicsThunk).
  void applyDesiredTopics() {
    auto desired = desired_topics_slot_.take();
    if (!desired) {
      return;
    }
    last_applied_desired_ = std::move(*desired);
    reconcileSubscriptions();
  }

  void teardown() {
    running_ = false;
    if (executor_) {
      executor_->cancel();
    }
    if (spinner_.joinable()) {
      spinner_.join();
    }
    subscriptions_.clear();
    subscription_types_.clear();
    if (executor_ && node_) {
      executor_->remove_node(node_);
    }
    executor_.reset();
    node_.reset();
    if (context_) {
      try {
        context_->shutdown("plugin stopping");
      } catch (...) {}
      context_.reset();
    }
    binding_cache_.clear();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::queue<PendingMessage> empty;
      std::swap(message_queue_, empty);
    }

    // --- Demand-driven per-topic subscription state ---
    demand_mode_ = false;
    last_applied_desired_.clear();
    discovered_topics_.clear();
    last_discovery_refresh_ = {};
    schema_by_type_.clear();
    warned_multi_type_topics_.clear();
    warned_schema_types_.clear();
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

  // [host GUI thread] pj.topic_subscription.v1's set_active_topics: stash the
  // full desired set into desired_topics_slot_ and return. ctx is the
  // Ros2StreamSource instance (DataSourceHandle::context()), per the ABI doc —
  // NOT the extension pointer returned by pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<Ros2StreamSource*>(ctx);
    std::set<std::string> topics;
    for (uint64_t i = 0; i < count; ++i) {
      topics.emplace(names[i].data, names[i].size);
    }
    self->desired_topics_slot_.set(std::move(topics));
    return true;
  }

  Ros2Dialog dialog_;
  std::vector<std::pair<std::string, std::string>> selected_topics_;
  std::string parser_config_json_;

  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::unordered_map<std::string, std::shared_ptr<rclcpp::GenericSubscription>> subscriptions_;
  // topic -> the type string actually passed to create_generic_subscription
  // for it (see subscribeTopic/resolveTypeForTopic). Kept in lockstep with
  // subscriptions_ (populated on subscribe, erased on unsubscribe).
  std::unordered_map<std::string, std::string> subscription_types_;
  std::thread spinner_;
  std::atomic<bool> running_{false};

  std::string parser_config_override_;
  std::mutex queue_mutex_;
  std::queue<PendingMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;

  // --- Demand-driven per-topic subscription (pj.topic_subscription.v1) ---

  // Decided once per start() by the first notify_available_topics attempt
  // (see onStart); false means an old host was detected and the legacy
  // selection-driven subscribe path is used instead, byte-for-byte.
  // Poll-thread-only after start() (decided synchronously inside onStart,
  // which runs before any poll — safe to read from onPoll afterward).
  bool demand_mode_ = false;

  // The most recent desired-topic set delivered by the host (via
  // applyDesiredTopics). Poll-thread-only. Retained so
  // reconcileSubscriptions() can pick up a topic that becomes discoverable
  // AFTER the host already asked for it (e.g. a late-starting publisher, or a
  // discovery refresh landing after a set_active_topics call).
  std::set<std::string> last_applied_desired_;

  // The streamer's OWN view of the ROS graph — topic name -> the one message
  // type it is published under (multi-type topics are excluded, see
  // splitTopicTypes). Refreshed on a ~1s throttle from onPoll (see
  // refreshDiscoveryAndAdvertise) plus once synchronously in onStart to seed
  // the very first advertise. Poll-thread-only.
  std::map<std::string, std::string> discovered_topics_;

  // Throttle gate for the streamer's discovery scan — get_topic_names_and_types()
  // walks the whole ROS graph and is too heavy to run on every ~50ms poll
  // tick. Poll-thread-only.
  std::chrono::steady_clock::time_point last_discovery_refresh_{};

  // Schema text cache, keyed by ROS 2 type name — buildRos2Schema() drives
  // typesupport introspection, which is not free; every topic sharing a type
  // (a common case) reuses the same synthesized schema. Poll-thread-only
  // (populated only from ensureSchemaForType, called from onStart/onPoll or
  // ensureBinding — the latter also poll-thread-only, per the onPoll
  // contract).
  std::map<std::string, std::string> schema_by_type_;

  // One-time-warning dedup sets, so a persistently multi-typed topic or a
  // type whose schema synthesis fails doesn't spam the message log on every
  // ~1s discovery refresh. Poll-thread-only.
  std::set<std::string> warned_multi_type_topics_;
  std::set<std::string> warned_schema_types_;

  // Cross-thread control-plane surface: setActiveTopicsThunk (entered on the
  // HOST GUI thread via pluginExtension/set_active_topics) writes;
  // applyDesiredTopics (poll thread, from onPoll) drains. Its own mutex — the
  // only new cross-thread surface this feature adds; discovered_topics_/
  // subscriptions_/binding_cache_ stay poll-thread-only, same as before.
  ros2_streamer::DesiredTopicsSlot desired_topics_slot_;
};

}  // namespace

// The inner DSO is loaded only by the proxy via dlopen + dlsym on private
// symbols. We deliberately do NOT export PJ_get_data_source_vtable nor
// PJ_get_dialog_vtable — those are the names the host's plugin scanner
// looks for, and the scanner walks the extension directory recursively.
// If the inner exported the standard names, every per-distro inner would
// be picked up as a top-level plugin (one per ROS distro) instead of the
// single proxy entry point. Renamed exports keep the inner invisible to
// the scanner while remaining reachable from the proxy.
extern "C" __attribute__((visibility("default"))) const PJ_data_source_vtable_t*
PJ_ros2_inner_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t* vt = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* {
        try {
          return new Ros2StreamSource();
        } catch (...) {
          return nullptr;
        }
      },
      kRos2Manifest);
  return vt;
}

extern "C" __attribute__((visibility("default"))) const PJ_dialog_vtable_t* PJ_ros2_inner_get_dialog_vtable() noexcept {
  static const PJ_dialog_vtable_t* vt = PJ::DialogPluginBase::vtableWithCreate([]() noexcept -> void* {
    try {
      return new Ros2Dialog();
    } catch (...) {
      return nullptr;
    }
  });
  return vt;
}

// `Ros2StreamSource::getDialog()` calls PJ::borrowDialog(dialog_), which
// expands to PJ::dialogVtableFor<Ros2Dialog>(). The PJ_DIALOG_PLUGIN macro
// is what normally specialises that template, but we don't use the macro
// here (it would also export the standard symbol the host scanner picks
// up). Provide the specialisation explicitly, pointing at the private
// inner getter.
namespace PJ {
template <>
[[maybe_unused]] inline const PJ_dialog_vtable_t* dialogVtableFor<Ros2Dialog>() noexcept {
  return PJ_ros2_inner_get_dialog_vtable();
}
}  // namespace PJ
