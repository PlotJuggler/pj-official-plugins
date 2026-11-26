#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <pj_websocket/source_connection.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_bridge_dialog.hpp"
#include "pj_bridge_manifest.hpp"
#include "pj_bridge_protocol.hpp"
#include "pj_bridge_topic_subscription.hpp"

namespace {

using namespace PJ::BridgeProtocol;

class PjBridgeSource : public PJ::StreamSourceBase {
 public:
  explicit PjBridgeSource(BridgeConnectionOptions options = {})
      : dialog_(options),
        options_(options),
        connection_(
            {.name = "PJ bridge",
             .connect = options.connect,
             .reconnect = std::nullopt,  // a lost connection stops the stream
             .inbound_queue_bytes = options.inbound_queue_bytes},
            {[this](bool /*reconnected: never, a loss is fatal*/) { beginDiscovery(); },
             [this](const std::string& text) { onTextMessage(text); },
             [this](std::string&& frame) { onBinaryFrame(frame); },
             [this](PJ::DataSourceMessageLevel level, const std::string& message) {
               runtimeHost().reportMessage(level, message);
             }}) {}

  /// The default services, plus the optional settings store the dialog remembers its last URL in.
  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = StreamSourceBase::bind(services);
    dialog_.endpoint().bind(services);
    return status;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    // SupportsPause: the host toolbar's connection-global pause()/resume(). PerTopicPause: demand-driven per-topic
    // subscription (pj.topic_subscription.v1).
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog | PJ::kCapabilitySupportsPause |
           PJ::kCapabilityPerTopicPause;
  }

  /// pj.topic_subscription.v1: the host pushes the full active-topic set here (see setActiveTopicsThunk).
  const void* pluginExtension(std::string_view id) override {
    if (id == PJ_TOPIC_SUBSCRIPTION_EXTENSION_V1) {
      static const PJ_topic_subscription_v1_t ext{sizeof(PJ_topic_subscription_v1_t), &setActiveTopicsThunk};
      return &ext;
    }
    return nullptr;
  }

  /// Any thread: the next onPoll() tells the server, once the connection is open, so the host
  /// must keep polling while paused (PJ4's worker loop polls until stop).
  PJ::Status pause() override {
    paused_ = true;
    return PJ::okStatus();
  }

  PJ::Status resume() override {
    paused_ = false;
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(
          std::string(
              "invalid config: expected a JSON object with a ws:// or wss:// \"url\" (or the legacy "
              "\"address\" and \"port\")"));
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }

    array_limit_ = PJ::sdk::arrayLimitFromJson(cfg);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // The saved selection is the subscribe set on a legacy host; on a demand-capable host only an optional advertise
    // filter (empty = advertise everything).
    selected_topics_.clear();
    if (cfg.contains("topics") && cfg["topics"].is_array()) {
      for (const auto& t : cfg["topics"]) {
        TopicInfo info;
        info.name = t.value("name", "");
        info.encoding = t.value("schema_encoding", "cdr");
        info.schema_name = t.value("schema_name", "");
        info.schema = t.value("schema_definition", "");
        if (!info.name.empty()) {
          selected_topics_.push_back(std::move(info));
        }
      }
    }

    // One socket serves dialog and streaming: adopt the dialog's open connection, else connect fresh (configured start
    // without the dialog). Either way start() returns before the connection completes; onPoll() drives
    // connecting -> discovery -> streaming.
    const bool adopted = connection_.adopt(dialog_.takeConnection());
    if (adopted) {
      // Lets demand mode list topics before our own get_topics round-trip; discarded on a legacy host.
      for (auto& info : dialog_.allDiscoveredTopics()) {
        advertised_topics_[info.name] = std::move(info);
      }
    } else if (auto status = connection_.start(dialog_.endpoint().str()); !status) {
      return status;
    }

    // Decided once per start by the first notify_available_topics attempt: a host without the tail slot returns an
    // error and the selection-driven legacy subscribe path is used; otherwise subscriptions are driven solely through
    // pj.topic_subscription.v1 (see onPoll / applyDesiredTopics).
    demand_mode_ = static_cast<bool>(refreshAdvertisedTopics());

    if (!demand_mode_) {
      // The dialog seed is for demand mode only: legacy schema lookup must resolve from selected_topics_ alone.
      advertised_topics_.clear();
      if (selected_topics_.empty()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "No topics selected — connected to a legacy PlotJuggler host, which cannot advertise "
            "topics on demand. Nothing will stream until the connection dialog is reopened and "
            "topics are selected.");
      }
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // A failed or lost connection stops the stream.
    if (auto status = connection_.poll(); !status) {
      return status;
    }
    if (!connection_.isOpen()) {
      return PJ::okStatus();  // nothing can be sent yet; host commands wait in their slot
    }
    if (const bool paused = paused_; paused != server_paused_) {
      server_paused_ = paused;
      connection_.sendText(buildRequest(paused ? "pause" : "resume", generateRequestId()));
    }
    if (awaiting_subscribe_response_) {
      if (subscribe_response_received_) {
        awaiting_subscribe_response_ = false;
      } else if (Clock::now() >= subscribe_deadline_) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "Subscribe response not received; parsers may lack schemas");
        awaiting_subscribe_response_ = false;
      }
    }

    // ~1 s: 30 polls at 33 ms
    if (++heartbeat_tick_ >= 30) {
      heartbeat_tick_ = 0;
      connection_.sendText(buildRequest("heartbeat", generateRequestId()));
    }

    if (demand_mode_) {
      // Only on a real advertise delta, so a desired-but-not-yet-advertised topic subscribes as soon as the server
      // announces it, without waiting for another host command.
      if (advertise_dirty_) {
        advertise_dirty_ = false;
        refreshAdvertisedTopicsAndWarn();
        reconcileSubscriptions();
      }
      applyDesiredTopics();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    connection_.stop();  // nothing from this session reaches the next
    awaiting_subscribe_response_ = false;
    bindings_.clear();
    selected_topics_.clear();
    advertised_topics_.clear();
    applied_.clear();
    last_subscribe_id_by_topic_.clear();
    last_applied_desired_.clear();
    demand_mode_ = false;
    advertise_dirty_ = false;
    server_info_logged_ = false;
    paused_ = false;
    server_paused_ = false;
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

 private:
  using Clock = std::chrono::steady_clock;

  // [poll thread] Fresh server session on an open socket: request what this mode needs before streaming.
  void beginDiscovery() {
    server_paused_ = false;  // onPoll() re-sends a pause() that came while connecting
    if (demand_mode_) {
      // Old servers ignore include_schemas and answer name+type only; the subscribe response is still the
      // authoritative schema source.
      // Order matters: opt into topics_changed BEFORE the snapshot. Otherwise a topic appearing between the two is
      // folded into the server's baseline and never pushed to us. A duplicate (pushed and in the snapshot) is harmless:
      // applyAdvertiseDelta upserts.
      connection_.sendText(buildDiscoveryRequest("subscribe_topic_updates"));
      connection_.sendText(buildDiscoveryRequest("get_topics"));
    } else {
      // The response carries the schemas needed for parsing; onPoll() waits for it until subscribe_deadline_.
      subscribe_response_received_ = false;
      std::vector<std::string> names;
      names.reserve(selected_topics_.size());
      for (const auto& info : selected_topics_) {
        names.push_back(info.name);
      }
      connection_.sendText(buildSubscribeMessage(names, generateRequestId()));
      subscribe_deadline_ = Clock::now() + options_.subscribe;
      awaiting_subscribe_response_ = true;
    }
  }

  // [poll thread] Decode one PJRB data frame and push each message to its topic's parser.
  void onBinaryFrame(const std::string& frame) {
    std::vector<RawMessage> messages;
    if (!parseBinaryFrame(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), messages, decompress_buffer_)) {
      return;
    }
    for (const auto& msg : messages) {
      auto it = bindings_.find(msg.topic_name);
      if (it != bindings_.end()) {
        // cdr_data points into decompress_buffer_, reused for the next frame, so copy. The fetcher must be idempotent
        // (ObjectIngestPolicy may dispatch it more than once, asynchronously) and its PayloadView anchor must keep the
        // bytes alive past the call.
        auto payload = std::make_shared<std::vector<uint8_t>>(msg.cdr_data, msg.cdr_data + msg.cdr_size);
        auto status = runtimeHost().pushMessage(
            it->second, PJ::Timestamp{msg.timestamp_ns},
            [payload]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{payload}; });
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning, "Failed to push message: " + status.error());
        }
      }
    }
  }

  // `subscribe` is additive: the server merges these into the session's set. The caller supplies the request id so it
  // can remember the last subscribe sent per topic (see failureIsStale).
  std::string buildSubscribeMessage(const std::vector<std::string>& topics, const std::string& request_id) const {
    nlohmann::json cmd;
    cmd["command"] = "subscribe";
    cmd["id"] = request_id;
    cmd["protocol_version"] = 1;
    cmd["topics"] = topics;
    return cmd.dump();
  }

  // The server silently ignores unknown names.
  std::string buildUnsubscribeMessage(const std::vector<std::string>& topics) const {
    nlohmann::json cmd;
    cmd["command"] = "unsubscribe";
    cmd["topics"] = topics;
    return cmd.dump();
  }

  // Old servers ignore include_schemas and answer with name+type-only entries.
  std::string buildDiscoveryRequest(const std::string& command) const {
    nlohmann::json cmd;
    cmd["command"] = command;
    cmd["id"] = generateRequestId();
    cmd["protocol_version"] = 1;
    cmd["include_schemas"] = true;
    return cmd.dump();
  }

  // Demand mode resolves from the advertise catalog; legacy mode has an empty catalog and falls back to the saved
  // selection.
  std::string resolveSchemaName(const std::string& topic) const {
    auto it = advertised_topics_.find(topic);
    if (it != advertised_topics_.end() && !it->second.schema_name.empty()) {
      return it->second.schema_name;
    }
    for (const auto& info : selected_topics_) {
      if (info.name == topic) {
        return info.schema_name;
      }
    }
    return {};
  }

  // stringField with a fallback for absent, empty or wrong-typed values (json::value would throw on the latter).
  static std::string stringFieldOr(const nlohmann::json& entry, const char* key, const char* fallback) {
    std::string value = stringField(entry, key);
    return value.empty() ? std::string(fallback) : value;
  }

  // [poll thread] An applied topic whose advertised type/schema changed keeps streaming bytes the old parser binding
  // would misparse. Dropping it from applied_/bindings_ makes the reconcile re-subscribe it, and the fresh response
  // re-binds with the new schema. The server treats re-subscribing a live topic idempotently.
  void dropRetypedApplied(const std::map<std::string, TopicInfo>& before) {
    for (const auto& topic : retypedTopics(before, advertised_topics_, applied_)) {
      applied_.erase(topic);
      bindings_.erase(topic);
      last_subscribe_id_by_topic_.erase(topic);
    }
  }

  // Names of the saved selection (empty = no filter). Derived from selected_topics_ so the two can't drift.
  std::vector<std::string> advertiseFilter() const {
    std::vector<std::string> names;
    names.reserve(selected_topics_.size());
    for (const auto& info : selected_topics_) {
      names.push_back(info.name);
    }
    return names;
  }

  // Schema bytes alias the TopicInfo::schema strings in advertised_topics_, which outlive the synchronous
  // notify_available_topics call.
  std::vector<PJ::AvailableTopic> buildAvailableTopics() const {
    const auto filter = advertiseFilter();
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(advertised_topics_.size());
    for (const auto& [name, info] : advertised_topics_) {
      if (!passesAdvertiseFilter(info, filter)) {
        continue;
      }
      const auto schema_bytes = reinterpret_cast<const uint8_t*>(info.schema.data());
      topics.push_back(
          PJ::AvailableTopic{
              name, info.encoding, info.schema_name, PJ::Span<const uint8_t>(schema_bytes, info.schema.size())});
    }
    return topics;
  }

  // The first call (onStart) also decides demand_mode_ via the returned status; later calls are made only when
  // demand_mode_ is already true.
  PJ::Status refreshAdvertisedTopics() {
    const auto topics = buildAvailableTopics();
    return runtimeHost().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  }

  void refreshAdvertisedTopicsAndWarn() {
    if (auto status = refreshAdvertisedTopics(); !status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Failed to advertise topics: " + status.error());
    }
  }

  // [poll thread] The single reconciliation point for demand mode: diffs applied_ against last_applied_desired_ over
  // the advertised, filter-passing topics and sends one subscribe and one unsubscribe. Runs after every advertise delta
  // and every new desired set, so desired-then-advertised and advertised-then-desired converge.
  void reconcileSubscriptions() {
    const auto advertised = filteredAdvertisedTopics(advertised_topics_, advertiseFilter());
    const auto diff = computeSubscriptionDiff(applied_, advertised, last_applied_desired_);

    if (!diff.to_unsubscribe.empty()) {
      connection_.sendText(buildUnsubscribeMessage(diff.to_unsubscribe));
      for (const auto& topic : diff.to_unsubscribe) {
        applied_.erase(topic);
        bindings_.erase(topic);
        last_subscribe_id_by_topic_.erase(topic);
      }
    }

    if (!diff.to_subscribe.empty()) {
      const std::string request_id = generateRequestId();
      connection_.sendText(buildSubscribeMessage(diff.to_subscribe, request_id));
      // Optimistic: a per-topic failure strips it back off (see handleSubscribeResponse) and it retries on the next
      // advertise delta, not in a tight loop. The id lets a stale failure be ignored (see failureIsStale).
      for (const auto& topic : diff.to_subscribe) {
        applied_.insert(topic);
        last_subscribe_id_by_topic_[topic] = request_id;
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

  // [poll thread] Dispatches by shape, not request id: a subscribe response has "schemas", a topics_changed push
  // "notification", a get_topics response a "topics" array. Anything else is ignored.
  void onTextMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    // The one hard compatibility gate: a newer protocol may carry semantics this client cannot interpret. Additive
    // features are feature-detected via the `server` capability list (see handleGetTopicsResponse).
    if (const auto newer = unsupportedProtocolVersion(json, kSupportedProtocolVersion)) {
      const std::string upgrade = "server speaks protocol_version " + std::to_string(*newer) +
                                  " but this plugin supports " + std::to_string(kSupportedProtocolVersion) +
                                  " — upgrade PlotJuggler (or run an older plotjuggler_bridge)";
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, upgrade);
      connection_.fail(upgrade);
      return;
    }

    if (json.contains("notification")) {
      if (json["notification"] == "topics_changed") {
        handleTopicsChanged(json);
      }
      return;
    }

    // Any response with "schemas" is a subscribe response, including the all-failed shape (status "error" +
    // ALL_SUBSCRIPTIONS_FAILED, schemas:{}): its failures must still strip applied_ or the rejected topics stay
    // "applied" forever. Binding is gated on applied_ membership and failures on staleness, so every status is safe.
    if (json.contains("schemas")) {
      handleSubscribeResponse(json);
      return;
    }

    if (json.contains("topics") && json["topics"].is_array()) {
      handleGetTopicsResponse(json);
      return;
    }
  }

  // Binds a parser per schema entry (the host reuses bindings by signature, so re-binding an unchanged topic is cheap)
  // and strips per-topic failures off applied_.
  void handleSubscribeResponse(const nlohmann::json& json) {
    const auto& schemas = json["schemas"];
    if (schemas.is_object()) {
      for (auto it = schemas.begin(); it != schemas.end(); ++it) {
        const std::string& topic_name = it.key();
        // A late response for a topic the host has since paused must not resurrect its binding: in-flight frames would
        // ingest into an inactive topic. Legacy mode never populates applied_, so the gate is demand-mode only.
        if (demand_mode_ && applied_.count(topic_name) == 0) {
          continue;
        }
        const auto& schema_obj = it.value();
        const std::string encoding = stringFieldOr(schema_obj, "encoding", "cdr");
        const std::string definition = stringField(schema_obj, "definition");
        const std::string schema_name = resolveSchemaName(topic_name);

        // parser_ros needs schema_encoding to pick the schema language: without it it defaults to ros2msg, which
        // miscompiles the FastDDS backend's omgidl schemas. use_embedded_timestamp is the key parser_ros reads;
        // use_timestamp is its older name, kept for other parsers.
        nlohmann::json parser_cfg;
        PJ::sdk::arrayLimitToJson(parser_cfg, array_limit_);
        parser_cfg["use_timestamp"] = use_timestamp_;
        parser_cfg["use_embedded_timestamp"] = use_timestamp_;
        parser_cfg["schema_encoding"] = encoding;
        const std::string parser_cfg_str = parser_cfg.dump();

        const auto schema_bytes = reinterpret_cast<const uint8_t*>(definition.data());
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = topic_name,
            .parser_encoding = encoding,
            .type_name = schema_name,
            .schema = PJ::Span<const uint8_t>(schema_bytes, definition.size()),
            .parser_config_json = parser_cfg_str,
        });
        if (binding) {
          bindings_[topic_name] = *binding;
        } else {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "Failed to create parser for " + topic_name + ": " + binding.error());
        }
      }
    }

    // The server forgets a failed subscribe (no sticky retry), so drop it from applied_; the next advertise delta
    // re-proposes it. A stale failure (see failureIsStale) is skipped: erasing applied_ would leave a live server-side
    // subscription no pause can stop.
    if (json.contains("failures") && json["failures"].is_array()) {
      const std::string response_id = stringField(json, "id");
      for (const auto& f : json["failures"]) {
        if (!f.is_object()) {
          continue;
        }
        const std::string topic = stringField(f, "topic");
        if (topic.empty() || failureIsStale(last_subscribe_id_by_topic_, topic, response_id)) {
          continue;
        }
        applied_.erase(topic);
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "Subscription failed: " + topic + " — " + stringFieldOr(f, "reason", "unknown"));
      }
    }

    subscribe_response_received_ = true;
  }

  // A real change arms a re-advertise + reconcile on the next poll.
  void handleTopicsChanged(const nlohmann::json& json) {
    std::vector<TopicInfo> added;
    if (json.contains("added") && json["added"].is_array()) {
      for (const auto& entry : json["added"]) {
        if (auto info = parseTopicEntry(entry)) {
          added.push_back(std::move(*info));
        }
      }
    }
    std::vector<std::string> removed;
    if (json.contains("removed") && json["removed"].is_array()) {
      for (const auto& name : json["removed"]) {
        if (name.is_string()) {
          removed.push_back(name.get<std::string>());
        }
      }
    }
    // Snapshot only applied_ entries: copying the whole catalog would clone every topic's multi-KB schema.
    std::map<std::string, TopicInfo> before_applied;
    for (const auto& name : applied_) {
      if (const auto it = advertised_topics_.find(name); it != advertised_topics_.end()) {
        before_applied[name] = it->second;
      }
    }
    if (applyAdvertiseDelta(advertised_topics_, added, removed)) {
      dropRetypedApplied(before_applied);
      advertise_dirty_ = true;
    }
  }

  // The authoritative full topic set: replaces the catalog wholesale; a real change arms a re-advertise + reconcile.
  // Also logs the server identity once and warns once when a capability the demand UX relies on is missing.
  void handleGetTopicsResponse(const nlohmann::json& json) {
    if (!server_info_logged_) {
      server_info_logged_ = true;
      const auto info = parseServerInfo(json);
      if (info) {
        std::string caps;
        for (const auto& capability : info->capabilities) {
          caps += (caps.empty() ? "" : ", ") + capability;
        }
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kInfo,
            "Connected to " + info->name + " " + info->version + " (capabilities: " + caps + ")");
      }
      // A pre-capability server (no `server` object) has none.
      if (demand_mode_ && (!info || !info->hasCapability("include_schemas"))) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "Server does not support schema advertisement (include_schemas): topic classification is "
            "degraded — TF may not auto-subscribe and 3D topics lose their icons until first subscribe. "
            "Upgrade plotjuggler_bridge to restore full functionality.");
      }
    }
    std::map<std::string, TopicInfo> fresh;
    for (const auto& entry : json["topics"]) {
      if (auto info = parseTopicEntry(entry)) {
        fresh[info->name] = std::move(*info);
      }
    }
    if (fresh != advertised_topics_) {
      const auto before = std::move(advertised_topics_);
      advertised_topics_ = std::move(fresh);
      dropRetypedApplied(before);
      advertise_dirty_ = true;
    }
  }

  // [host GUI thread] set_active_topics: stash the full desired set in desired_topics_slot_. `ctx` is the
  // PjBridgeSource (DataSourceHandle::context()), not the extension pointer from pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<PjBridgeSource*>(ctx);
    self->desired_topics_slot_.set(PJ::sdk::stringSetFromViews(names, count));
    return true;
  }

  PjBridgeDialog dialog_;
  BridgeConnectionOptions options_;

  // The connection of this start; its handlers run on the poll thread, from onPoll().
  PJ::WebSocket::SourceConnection connection_;
  PJ::sdk::ArrayLimit array_limit_;
  bool use_timestamp_ = false;

  // Legacy subscribe list and demand-mode advertise filter (see onStart); per-topic schema info serves the legacy
  // schema_name lookup.
  std::vector<TopicInfo> selected_topics_;

  std::map<std::string, PJ::ParserBindingHandle> bindings_;  // poll-thread-only

  // Legacy mode: the subscribe response is in flight, until it arrives or subscribe_deadline_.
  bool awaiting_subscribe_response_ = false;
  Clock::time_point subscribe_deadline_{};

  std::vector<uint8_t> decompress_buffer_;
  int heartbeat_tick_ = 0;
  std::atomic<bool> paused_{false};  ///< written by the host's pause()/resume(), read on the poll thread
  bool server_paused_ = false;       ///< poll-thread-only: what the server was last told
  // Poll-thread-only: every response is handled from connection_.poll().
  bool subscribe_response_received_ = false;

  // Demand-driven per-topic subscription (pj.topic_subscription.v1). Poll-thread-only unless noted.

  // Decided once per start() by the first notify_available_topics attempt.
  bool demand_mode_ = false;

  // Set when the catalog changes, so onPoll re-advertises and reconciles once per poll even across a burst of deltas.
  bool advertise_dirty_ = false;

  // Topic name -> type + encoding + schema, from get_topics / topics_changed. Empty in legacy mode (resolveSchemaName
  // then falls back to selected_topics_).
  std::map<std::string, TopicInfo> advertised_topics_;

  // Live subscriptions. Grown optimistically at subscribe, shrunk by unsubscribe and per-topic failures.
  std::set<std::string> applied_;

  bool server_info_logged_ = false;

  // Id of the last subscribe naming each topic, so a stale failure is ignored (see failureIsStale). Pruned on
  // unsubscribe/retype; cleared in onStop.
  std::map<std::string, std::string> last_subscribe_id_by_topic_;

  // Retained so a topic advertised after the host asked for it is subscribed without another set_active_topics.
  std::set<std::string> last_applied_desired_;

  // Written by setActiveTopicsThunk (host GUI thread), drained by applyDesiredTopics; the only cross-thread surface
  // besides connection_'s queue.
  DesiredTopicsSlot desired_topics_slot_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(PjBridgeSource, kPjBridgeManifest)

PJ_DIALOG_PLUGIN(PjBridgeDialog, kPjBridgeManifest)
