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
#include <nlohmann/json.hpp>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
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
  // Keeps the rcl buffer alive for any deferred FetchMessageData pull; also the PayloadView anchor.
  std::shared_ptr<const rclcpp::SerializedMessage> msg;
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

  /// pj.topic_subscription.v1: the host pushes the full active-topic set here (see setActiveTopicsThunk).
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
    // Forward even an empty config: dialog_.loadConfig() also starts topic discovery, which a fresh open needs.
    (void)dialog_.loadConfig(config_json);
    // The host persists parser config (e.g. "use_embedded_timestamp") under "_parser_config".
    parser_config_override_ = PJ::sdk::parserConfigOverride(config_json);
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
    // The selection is optional: with demand subscriptions an empty one means "advertise everything". A legacy host
    // with an empty selection only warns (see below, after demand_mode_ is decided).

    // Passed to every ensureBinding; parser_ros::loadConfig ignores unknown keys.
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

      // This brand-new Context/Node has seen no DDS discovery traffic yet, so the scan will likely be empty. Only then
      // fall back to the saved selection's (name, type) pairs, so the first advertise is not empty while the host
      // waits for the next ~1 s refresh (see refreshDiscoveryAndAdvertise). An empty first advertise is still a valid
      // probe for demand_mode_.
      discovered_topics_ = scanDiscovery();
      last_discovery_refresh_ = std::chrono::steady_clock::now();
      if (discovered_topics_.empty()) {
        for (const auto& [name, type] : selected_topics_) {
          discovered_topics_[name] = type;
        }
        // Advertise-only: no publisher is discoverable yet, so subscribing now would lock in an unadaptable default
        // QoS (see reconcileSubscriptions). Cleared by the first genuine graph scan.
        discovery_seeded_from_selection_ = !discovered_topics_.empty();
      }

      // Decided once per start by the first advertise attempt: a host without the notify_available_topics tail slot
      // returns an error and the selection-driven legacy subscribe path is used. Otherwise the host is re-notified on
      // every discovery delta (see refreshDiscoveryAndAdvertise) and drives subscriptions solely through
      // pj.topic_subscription.v1 (see onPoll / applyDesiredTopics); there is no auto-subscribe here.
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
    // Both calls are non-blocking; desired-then-discovered and discovered-then-desired converge on
    // reconcileSubscriptions().
    if (demand_mode_) {
      refreshDiscoveryAndAdvertise();
      applyDesiredTopics();
    }

    auto batch = message_queue_.drain();

    while (!batch.empty()) {
      auto& msg = batch.front();
      auto* binding = ensureBinding(msg.topic);
      if (binding != nullptr) {
        // Zero-copy: the closure captures the SerializedMessage shared_ptr, which owns the rcl buffer and is the
        // BufferAnchor, so the buffer survives however long the host's ObjectIngestPolicy defers the pull.
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
  void enqueueMessage(const std::string& topic, std::shared_ptr<const rclcpp::SerializedMessage> msg) {
    if (!msg) {
      return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    PendingMessage pending{
        .topic = topic,
        .msg = std::move(msg),
        .timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
    };

    message_queue_.push(std::move(pending));
  }

  // subscription_types_ is the single source of truth for the type that created the live GenericSubscription (see
  // subscribeTopic). Not re-derived from selected_topics_/discovered_topics_, which may hold a different type for the
  // same name (e.g. a stale selection saved before a retype): the parser schema must match the bytes actually being
  // decoded.
  std::string resolveTypeForTopic(const std::string& topic) const {
    auto it = subscription_types_.find(topic);
    return it != subscription_types_.end() ? it->second : std::string{};
  }

  PJ::ParserBindingHandle* ensureBinding(const std::string& topic) {
    const std::string type_name = resolveTypeForTopic(topic);
    if (type_name.empty()) {
      return nullptr;
    }

    auto it = binding_cache_.find(topic);
    if (it != binding_cache_.end()) {
      // Reuse only while the subscription's type matches the cached binding's: a topic can be unsubscribed, retyped
      // and re-subscribed within one session, and the new CDR bytes through the old parser would decode garbage. There
      // is no unbind ABI, so the superseded binding stays alive host-side, idle.
      if (binding_type_by_topic_[topic] == type_name) {
        return &it->second;
      }
      binding_cache_.erase(it);
    }

    const std::string* schema = ensureSchemaForType(type_name);
    if (schema == nullptr) {
      return nullptr;  // warned once by ensureSchemaForType
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
    binding_type_by_topic_[topic] = type_name;
    return &iter->second;
  }

  // Cached per type, shared by advertise and subscribe. Returns nullptr after a one-time warning if introspection
  // cannot resolve the type, so a persistently unresolvable type does not warn on every message.
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

  // Adapts QoS to the publishers visible within `qos_wait`. The legacy onStart path waits 2 s (fresh Context, DDS
  // discovery may not have propagated); the demand-mode reconcile on the poll thread passes 0 (sample once, never
  // block).
  void subscribeTopic(const std::string& topic, const std::string& type, std::chrono::milliseconds qos_wait) {
    const auto qos = ros2_streamer::adaptQosWaitingForPublishers(*node_, topic, qos_wait);
    if (node_->count_publishers(topic) == 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "No publishers visible for " + topic + " within discovery timeout — using default QoS");
    }

    auto callback = [this, topic](std::shared_ptr<const rclcpp::SerializedMessage> msg) {
      enqueueMessage(topic, std::move(msg));
    };

    auto subscription = node_->create_generic_subscription(topic, type, qos, callback);
    subscriptions_.emplace(topic, std::move(subscription));
    subscription_types_[topic] = type;
  }

  // [poll thread] Excludes multi-type topics (warned once each, see splitTopicTypes).
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

  // Restricted by the dialog's saved selection, which is dual-purpose: the subscribe list on a legacy host (the loop in
  // onStart), an optional advertise filter in demand mode (empty = advertise everything). Schema bytes alias
  // schema_by_type_'s cached strings, which outlive the synchronous notify_available_topics call.
  std::vector<PJ::AvailableTopic> buildAvailableTopics() {
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(discovered_topics_.size());
    for (const auto& [topic, type] : discovered_topics_) {
      if (!ros2_streamer::passesAdvertiseFilter(topic, type, selected_topics_)) {
        continue;
      }
      const std::string* schema = ensureSchemaForType(type);
      if (schema == nullptr) {
        continue;  // warned once by ensureSchemaForType
      }
      topics.push_back(
          PJ::AvailableTopic{
              topic, "ros2msg", type,
              PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(schema->data()), schema->size())});
    }
    return topics;
  }

  // The first call (onStart) also decides demand_mode_ via the returned status; later calls (after a discovery delta)
  // are made only when demand_mode_ is already true.
  PJ::Status refreshAdvertisedTopics() {
    const auto topics = buildAvailableTopics();
    return runtimeHost().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  }

  // For steady-state calls, where a failure only merits a warning (the first call in onStart() decides demand_mode_).
  void refreshAdvertisedTopicsAndWarn() {
    if (auto status = refreshAdvertisedTopics(); !status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Failed to advertise topics: " + status.error());
    }
  }

  // [poll thread] ~1 s ROS-graph scan by the streamer's own node (the dialog's node is gone by the time the streamer
  // starts). Re-advertises whenever the discovered set or a topic's type changes, and reconciles so a topic that just
  // became discoverable but was already desired subscribes without another set_active_topics.
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

    // The first genuine scan replaces the advertise-only seed (see onStart) and unblocks subscribing. Reconcile must
    // run even when the scan equals the seeded map, or a desired seeded topic never subscribes.
    const bool was_seeded = discovery_seeded_from_selection_;
    discovery_seeded_from_selection_ = false;

    if (!was_seeded && topics == discovered_topics_) {
      return;
    }
    const bool advertise_changed = topics != discovered_topics_;
    discovered_topics_ = std::move(topics);

    if (advertise_changed) {
      refreshAdvertisedTopicsAndWarn();
    }
    reconcileSubscriptions();
  }

  // [poll thread] The single reconciliation point for demand mode: diffs live subscriptions against
  // last_applied_desired_ over the discovered, filter-passing topics. Runs after every new desired set and every
  // discovery delta, so desired-then-discovered and discovered-then-desired converge.
  void reconcileSubscriptions() {
    // Never subscribe off the saved-selection seed (see onStart): no publisher is discoverable, so qos_wait=0 would
    // create a default-QoS subscription that a BEST_EFFORT/TRANSIENT_LOCAL publisher never matches, and nothing would
    // recreate it. Real subscriptions wait for the first genuine scan (~1 s).
    if (discovery_seeded_from_selection_) {
      return;
    }

    // A topic the advertise filter hides must not be subscribed (see filteredDiscoveredTopics); a live subscription to
    // a now-filtered topic is dropped by the same diff.
    const auto subscribable = ros2_streamer::filteredDiscoveredTopics(discovered_topics_, selected_topics_);

    // topic -> created-with type: the diff recreates a subscription whose discovered type changed.
    const std::map<std::string, std::string> current(subscription_types_.begin(), subscription_types_.end());
    const auto diff = ros2_streamer::computeRos2SubscriptionDiff(current, subscribable, last_applied_desired_);

    for (const auto& topic : diff.to_unsubscribe) {
      // Erasing drops the shared_ptr, which unsubscribes in DDS. binding_cache_ is deliberately kept: bindings are
      // host-side state, an idle one costs nothing, and re-subscribing reuses it.
      subscriptions_.erase(topic);
      subscription_types_.erase(topic);
    }

    for (const auto& topic : diff.to_subscribe) {
      const auto it = subscribable.find(topic);
      if (it == subscribable.end()) {
        continue;  // should not happen: diff only proposes subscribable topics
      }
      try {
        // onPoll must not block: the topic just came out of a completed scan, so its publisher's QoS is normally
        // resolvable already; qos_wait=0 samples once instead of polling for up to 2 s.
        subscribeTopic(topic, it->second, std::chrono::milliseconds(0));
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "Failed to subscribe to " + topic + ": " + e.what());
      }
    }
  }

  // [poll thread] Latest-wins slot, see setActiveTopicsThunk.
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
    binding_type_by_topic_.clear();
    message_queue_.clear();

    demand_mode_ = false;
    last_applied_desired_.clear();
    discovered_topics_.clear();
    discovery_seeded_from_selection_ = false;
    last_discovery_refresh_ = {};
    schema_by_type_.clear();
    warned_multi_type_topics_.clear();
    warned_schema_types_.clear();
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

  // [host GUI thread] set_active_topics: stash the full desired set in desired_topics_slot_. `ctx` is the
  // Ros2StreamSource (DataSourceHandle::context()), not the extension pointer from pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<Ros2StreamSource*>(ctx);
    self->desired_topics_slot_.set(PJ::sdk::stringSetFromViews(names, count));
    return true;
  }

  Ros2Dialog dialog_;
  std::vector<std::pair<std::string, std::string>> selected_topics_;
  std::string parser_config_json_;

  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<rclcpp::Node> node_;
  std::unique_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::unordered_map<std::string, std::shared_ptr<rclcpp::GenericSubscription>> subscriptions_;
  // topic -> type passed to create_generic_subscription (see resolveTypeForTopic); in lockstep with subscriptions_.
  std::unordered_map<std::string, std::string> subscription_types_;
  std::thread spinner_;
  std::atomic<bool> running_{false};

  std::string parser_config_override_;
  PJ::sdk::DrainQueue<PendingMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
  // topic -> type its binding_cache_ entry was created for, so ensureBinding() can invalidate on a retype.
  // Poll-thread-only, in lockstep with binding_cache_.
  std::unordered_map<std::string, std::string> binding_type_by_topic_;

  // Demand-driven per-topic subscription (pj.topic_subscription.v1). Poll-thread-only unless noted.

  // Decided once per start() by the first notify_available_topics attempt (see onStart); false means an old host and
  // the legacy selection-driven subscribe path. Set synchronously in onStart, before any poll.
  bool demand_mode_ = false;

  // Retained so reconcileSubscriptions() picks up a topic that becomes discoverable after the host asked for it
  // (late-starting publisher, or a discovery refresh landing after set_active_topics).
  std::set<std::string> last_applied_desired_;

  // The streamer's own view of the ROS graph: topic -> its one type (multi-type topics excluded, see splitTopicTypes).
  // Refreshed ~1 s from onPoll, and once in onStart to seed the first advertise.
  std::map<std::string, std::string> discovered_topics_;

  // True while discovered_topics_ holds the advertise-only fallback seeded from the saved selection in onStart.
  // Seeded entries must never be subscribed (see reconcileSubscriptions). Cleared by the first genuine scan, which then
  // reconciles unconditionally.
  bool discovery_seeded_from_selection_ = false;

  // get_topic_names_and_types() walks the whole ROS graph: too heavy for every ~50 ms poll.
  std::chrono::steady_clock::time_point last_discovery_refresh_{};

  // Per ROS 2 type: buildRos2Schema() runs typesupport introspection, and topics often share a type.
  std::map<std::string, std::string> schema_by_type_;

  // So a persistently multi-typed topic or a failing schema build warns once, not on every ~1 s refresh.
  std::set<std::string> warned_multi_type_topics_;
  std::set<std::string> warned_schema_types_;

  // Written by setActiveTopicsThunk (host GUI thread), drained by applyDesiredTopics; the only cross-thread surface.
  ros2_streamer::DesiredTopicsSlot desired_topics_slot_;
};

}  // namespace

// The proxy loads the inner DSO via dlopen + dlsym on private symbols. It must not export PJ_get_data_source_vtable or
// PJ_get_dialog_vtable: the host scanner walks the extension directory recursively and would pick up every per-distro
// inner as a top-level plugin instead of the single proxy entry point.
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

// Ros2StreamSource::getDialog() uses PJ::borrowDialog(dialog_), which needs dialogVtableFor<Ros2Dialog>(). The
// PJ_DIALOG_PLUGIN macro normally specialises it but would also export the standard symbol, so specialise explicitly
// to the private inner getter.
namespace PJ {
template <>
[[maybe_unused]] inline const PJ_dialog_vtable_t* dialogVtableFor<Ros2Dialog>() noexcept {
  return PJ_ros2_inner_get_dialog_vtable();
}
}  // namespace PJ
