#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_bridge_dialog.hpp"
#include "pj_bridge_manifest.hpp"
#include "pj_bridge_protocol.hpp"
#include "pj_bridge_topic_subscription.hpp"
#include "pj_bridge_websocket.hpp"

namespace {

using namespace PJ::BridgeProtocol;

class PjBridgeSource : public PJ::StreamSourceBase {
 public:
  explicit PjBridgeSource(BridgeConnectionOptions options = {}) : dialog_(options), options_(options) {}

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    // kCapabilitySupportsPause keeps the connection-global pause()/resume() wire
    // commands the host toolbar drives; kCapabilityPerTopicPause adds the
    // demand-driven per-topic subscription control (pj.topic_subscription.v1).
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog | PJ::kCapabilitySupportsPause |
           PJ::kCapabilityPerTopicPause;
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

  /// Remembered while connecting; beginDiscovery() applies it once the socket opens.
  PJ::Status pause() override {
    paused_ = true;
    if (socket_ && socket_->isOpen()) {
      socket_->sendText(buildRequest("pause", generateRequestId()));
    }
    return PJ::okStatus();
  }

  PJ::Status resume() override {
    paused_ = false;
    if (socket_ && socket_->isOpen()) {
      socket_->sendText(buildRequest("resume", generateRequestId()));
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(
          std::string(
              "invalid config: expected a JSON object with a ws:// or wss:// \"url\", or "
              "\"address\" and \"port\""));
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }

    endpoint_ = dialog_.endpoint();
    array_limit_ = PJ::sdk::arrayLimitFromJson(cfg);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Read the saved topic selection. On a legacy host this is the subscribe
    // set (exactly as before); on a demand-capable host it is only an OPTIONAL
    // advertise filter (empty = advertise everything).
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

    // Adopt the dialog's connection when it hands over an open one (one socket serves both
    // dialog and streaming, as in the original plugin); otherwise connect fresh, e.g. for a
    // configured start without the dialog. Either way start() returns before the connection
    // completes: onPoll() drives connecting -> discovery -> streaming.
    phase_ = ConnectionPhase::kConnecting;
    auto handoff = dialog_.takeConnection();
    const bool adopted = handoff.socket && handoff.socket->isOpen();
    if (adopted) {
      socket_ = std::move(handoff.socket);
      events_ = std::move(handoff.events);
      // Seed the advertise catalog from the topics the dialog already discovered, so demand mode
      // can list them immediately instead of waiting for our own get_topics round-trip.
      // Discarded if the host turns out to be legacy.
      for (auto& info : dialog_.allDiscoveredTopics()) {
        advertised_topics_[info.name] = std::move(info);
      }
    } else {
      events_ = std::make_shared<BridgeEventQueue>(options_.inbound_queue_bytes);
      socket_ = std::make_unique<BridgeWebSocket>();
      if (auto status = socket_->start(endpoint_, events_); !status) {
        return PJ::unexpected("failed to connect to PJ bridge at " + endpoint_ + ": " + status.error());
      }
      connect_deadline_ = Clock::now() + options_.connect;
    }

    // Demand mode is decided ONCE per start by the first notify_available_topics
    // attempt: a host that lacks the tail slot (old PJ4) returns an error, so a
    // new plugin degrades to the legacy selection-driven subscribe path below,
    // byte-for-byte. A host that accepts it drives subscriptions solely through
    // pj.topic_subscription.v1 (see onPoll / applyDesiredTopics).
    demand_mode_ = static_cast<bool>(refreshAdvertisedTopics());

    if (!demand_mode_) {
      // --- Legacy host: preserve the original behavior. ---
      // The dialog seed is only for demand mode; drop it so the legacy schema
      // lookup resolves from selected_topics_ alone.
      advertised_topics_.clear();
      if (selected_topics_.empty()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "No topics selected — connected to a legacy PlotJuggler host, which cannot advertise "
            "topics on demand. Nothing will stream until the connection dialog is reopened and "
            "topics are selected.");
      }
    }

    if (adopted) {
      beginDiscovery();
    }
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    pumpEvents();
    if (phase_ == ConnectionPhase::kConnecting && Clock::now() >= connect_deadline_) {
      failConnection("timed out");
    }
    if (!fatal_error_.empty()) {
      return PJ::unexpected(fatal_error_);  // stops the stream; latched until stop()
    }
    if (phase_ == ConnectionPhase::kConnecting) {
      return PJ::okStatus();  // nothing can be sent yet; host commands wait in their slot
    }
    if (phase_ == ConnectionPhase::kDiscovery && !demand_mode_) {
      if (subscribe_response_received_) {
        phase_ = ConnectionPhase::kStreaming;
      } else if (Clock::now() >= subscribe_deadline_) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "Subscribe response not received; parsers may lack schemas");
        phase_ = ConnectionPhase::kStreaming;
      }
    }

    // Heartbeat: send every ~1s (30 polls at 33ms each)
    if (++heartbeat_tick_ >= 30) {
      heartbeat_tick_ = 0;
      socket_->sendText(buildRequest("heartbeat", generateRequestId()));
    }

    if (demand_mode_) {
      // Re-advertise + reconcile only on a genuine advertise delta, so a
      // desired-but-not-yet-advertised topic subscribes as soon as the server
      // announces it (topics_changed) without waiting for another host command.
      if (advertise_dirty_) {
        advertise_dirty_ = false;
        refreshAdvertisedTopicsAndWarn();
        reconcileSubscriptions();
      }
      // Apply the host's latest desired active-topic set, if it wrote one.
      applyDesiredTopics();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (socket_) {
      socket_->stop();
      socket_.reset();
    }
    events_.reset();  // a fresh queue per start: nothing from this session reaches the next
    phase_ = ConnectionPhase::kConnecting;
    bindings_.clear();
    selected_topics_.clear();
    advertised_topics_.clear();
    applied_.clear();
    last_subscribe_id_by_topic_.clear();
    last_applied_desired_.clear();
    demand_mode_ = false;
    advertise_dirty_ = false;
    fatal_error_.clear();
    server_info_logged_ = false;
    reported_dropped_frames_ = 0;
    paused_ = false;
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

 private:
  using Clock = std::chrono::steady_clock;

  static constexpr std::chrono::seconds kDropWarningInterval{5};

  /// Where the connection is. Advances only from onPoll() (or onStart() for an adopted socket).
  enum class ConnectionPhase {
    kConnecting,  ///< waiting for kOpen until connect_deadline_
    kDiscovery,   ///< open; the legacy subscribe response is in flight (demand mode stays here)
    kStreaming,   ///< legacy mode: the subscribe response arrived or timed out
  };

  // [start or poll thread] The socket is open: request what this mode needs before streaming.
  void beginDiscovery() {
    phase_ = ConnectionPhase::kDiscovery;
    if (demand_mode_) {
      // Ask the server for the authoritative full topic set WITH schemas, and
      // opt into topics_changed pushes (also WITH schemas). Old servers ignore
      // include_schemas and just answer name+type — the subscribe response is
      // still the authoritative schema source, so binding works regardless.
      // Order matters: opt into topics_changed pushes BEFORE taking the
      // snapshot. The reverse leaves a gap — a topic appearing between the
      // snapshot and the opt-in is folded into the server's baseline without
      // ever being pushed to us. A duplicate (pushed AND in the snapshot) is
      // harmless: applyAdvertiseDelta upserts.
      socket_->sendText(buildDiscoveryRequest("subscribe_topic_updates"));
      socket_->sendText(buildDiscoveryRequest("get_topics"));
    } else {
      // Legacy: subscribe to the selected topics — the response carries the schemas needed for
      // parsing. onPoll() waits for it until subscribe_deadline_.
      subscribe_response_received_ = false;
      std::vector<std::string> names;
      names.reserve(selected_topics_.size());
      for (const auto& info : selected_topics_) {
        names.push_back(info.name);
      }
      socket_->sendText(buildSubscribeMessage(names, generateRequestId()));
      subscribe_deadline_ = Clock::now() + options_.subscribe;
    }
    if (paused_) {
      socket_->sendText(buildRequest("pause", generateRequestId()));  // pause() came while connecting
    }
  }

  /// The first stream-stopping failure wins; a close after a timeout does not replace it.
  void fail(std::string message) {
    if (fatal_error_.empty()) {
      fatal_error_ = std::move(message);
    }
  }

  void failConnection(const std::string& reason) {
    const char* what = phase_ == ConnectionPhase::kConnecting ? "failed to connect to PJ bridge at "
                                                              : "lost connection to PJ bridge at ";
    fail(what + endpoint_ + (reason.empty() ? std::string{} : ": " + reason));
  }

  // [poll thread] Apply every queued transport event in arrival order. Parser bindings are only
  // created and read here, never on a transport thread.
  void pumpEvents() {
    if (!events_) {
      return;
    }
    // Pop each event as it is applied, so a long batch frees its frames as it goes.
    auto pending = events_->drain();
    while (!pending.empty()) {
      const BridgeWebSocketEvent event = std::move(pending.front());
      pending.pop_front();
      switch (event.type) {
        case BridgeWebSocketEvent::Type::kOpen:
          if (phase_ == ConnectionPhase::kConnecting) {
            beginDiscovery();
          }
          break;
        case BridgeWebSocketEvent::Type::kText:
          // Wire data can be arbitrarily malformed (nlohmann accessors throw on wrong-typed
          // fields): drop the frame with a warning, never fail the host's poll.
          try {
            onTextMessage(event.payload);
          } catch (const std::exception& e) {
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kWarning, std::string("Dropped malformed control frame: ") + e.what());
          }
          break;
        case BridgeWebSocketEvent::Type::kBinary:
          onBinaryFrame(event.payload);
          break;
        case BridgeWebSocketEvent::Type::kClosed:
          failConnection(event.payload.empty() ? "closed by the server" : event.payload);
          break;
        case BridgeWebSocketEvent::Type::kError:
          failConnection(event.payload);
          break;
      }
    }
    reportDroppedData();
  }

  // [poll thread] The queue drops the oldest data frames when this thread falls behind (see
  // BridgeEventQueue). Warn at the first drop, then at most once per kDropWarningInterval, each
  // time with the running total of this session.
  void reportDroppedData() {
    const auto dropped = events_->dropped();
    if (dropped.frames == reported_dropped_frames_) {
      return;
    }
    const auto now = Clock::now();
    if (reported_dropped_frames_ > 0 && now - last_drop_warning_ < kDropWarningInterval) {
      return;
    }
    reported_dropped_frames_ = dropped.frames;
    last_drop_warning_ = now;
    char mebibytes[32];
    std::snprintf(mebibytes, sizeof(mebibytes), "%.1f", static_cast<double>(dropped.bytes) / (1024.0 * 1024.0));
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kWarning, "PlotJuggler fell behind the PJ bridge stream at " + endpoint_ +
                                                  " and dropped the oldest " + std::to_string(dropped.frames) +
                                                  " data frames (" + mebibytes +
                                                  " MiB) so far; the plots have gaps where they were.");
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
        // msg.cdr_data points into decompress_buffer_ which is reused for the
        // next frame, so we own the bytes through a shared_ptr-owned buffer.
        // The fetcher must be idempotent (ObjectIngestPolicy may dispatch
        // it more than once / asynchronously) and must return a PayloadView
        // whose anchor keeps the bytes alive past the call.
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

  // Build a subscribe frame for a set of topic names. `subscribe` is ADDITIVE:
  // the server merges these into the session's set without disturbing the rest.
  // The caller supplies the request id so it can remember which subscribe was
  // the LAST one sent per topic (see failureIsStale).
  std::string buildSubscribeMessage(const std::vector<std::string>& topics, const std::string& request_id) const {
    nlohmann::json cmd;
    cmd["command"] = "subscribe";
    cmd["id"] = request_id;
    cmd["protocol_version"] = 1;
    cmd["topics"] = topics;
    return cmd.dump();
  }

  // Build an unsubscribe frame for a set of topic names. Unknown names are
  // silently ignored by the server.
  std::string buildUnsubscribeMessage(const std::vector<std::string>& topics) const {
    nlohmann::json cmd;
    cmd["command"] = "unsubscribe";
    cmd["topics"] = topics;
    return cmd.dump();
  }

  // Build a get_topics / subscribe_topic_updates frame that opts into per-topic
  // schemas. Old servers ignore include_schemas (graceful degradation to
  // name+type-only entries).
  std::string buildDiscoveryRequest(const std::string& command) const {
    nlohmann::json cmd;
    cmd["command"] = command;
    cmd["id"] = generateRequestId();
    cmd["protocol_version"] = 1;
    cmd["include_schemas"] = true;
    return cmd.dump();
  }

  // Resolve the ROS 2 type name for `topic`. In demand mode it comes from the
  // advertise catalog (get_topics/topics_changed); in legacy mode the catalog is
  // empty, so this falls back to the saved selection — identical to the original
  // per-topic schema_name lookup.
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

  // stringField with a non-empty fallback (wire fields default like json::value
  // used to, but wrong-typed values degrade instead of throwing).
  static std::string stringFieldOr(const nlohmann::json& entry, const char* key, const char* fallback) {
    std::string value = stringField(entry, key);
    return value.empty() ? std::string(fallback) : value;
  }

  // [poll thread] An applied topic whose advertised type/schema CHANGED keeps
  // streaming bytes the old parser binding would misparse. Drop it from
  // applied_/bindings_ so the reconcile armed by this catalog change
  // re-subscribes it: the fresh subscribe response re-binds with the new schema
  // (the host mints a fresh binding when the signature changed). The server
  // treats the re-subscribe of a still-live topic idempotently.
  void dropRetypedApplied(const std::map<std::string, TopicInfo>& before) {
    for (const auto& topic : retypedTopics(before, advertised_topics_, applied_)) {
      applied_.erase(topic);
      bindings_.erase(topic);
      last_subscribe_id_by_topic_.erase(topic);
    }
  }

  // The advertise filter: just the names of the saved selection (empty = no
  // filter, advertise everything). Derived from selected_topics_ so the two
  // can't drift; recomputed per advertise/reconcile, both cold paths.
  std::vector<std::string> advertiseFilter() const {
    std::vector<std::string> names;
    names.reserve(selected_topics_.size());
    for (const auto& info : selected_topics_) {
      names.push_back(info.name);
    }
    return names;
  }

  // Build the AvailableTopic list for notify_available_topics from the advertise
  // catalog, restricted by the (optional) advertise filter. Schema bytes alias
  // the TopicInfo::schema strings held in advertised_topics_, which outlive the
  // synchronously-consumed notify_available_topics call. Mirrors the ros2 /
  // foxglove parameter shape: {topic, parser_encoding, type_name, schema}.
  std::vector<PJ::AvailableTopic> buildAvailableTopics() const {
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(advertised_topics_.size());
    for (const auto& [name, info] : advertised_topics_) {
      if (!passesAdvertiseFilter(name, advertiseFilter())) {
        continue;
      }
      const auto schema_bytes = reinterpret_cast<const uint8_t*>(info.schema.data());
      topics.push_back(
          PJ::AvailableTopic{
              name, info.encoding, info.schema_name, PJ::Span<const uint8_t>(schema_bytes, info.schema.size())});
    }
    return topics;
  }

  // Advertise the current (filtered) full topic set to the host. The FIRST call,
  // from onStart(), also decides demand_mode_ via the returned status. Later
  // calls (from onPoll, after an advertise delta) are only made when demand_mode_
  // is already true — no point re-probing a host that has already been found to
  // lack the slot.
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

  // [poll thread] The single reconciliation point for demand mode: diff the
  // applied subscriptions against last_applied_desired_ over the currently
  // advertised (and filter-passing) topics, sending ONE additive subscribe for
  // the newly wanted topics and ONE unsubscribe for the dropped ones. Called
  // after every advertise delta (onPoll) AND when a new desired set lands
  // (applyDesiredTopics) — so desired-then-advertised and advertised-then-desired
  // converge on one code path. Empty send lists are suppressed.
  void reconcileSubscriptions() {
    const auto advertised = filteredAdvertisedTopics(advertised_topics_, advertiseFilter());
    const auto diff = computeSubscriptionDiff(applied_, advertised, last_applied_desired_);

    if (!diff.to_unsubscribe.empty()) {
      socket_->sendText(buildUnsubscribeMessage(diff.to_unsubscribe));
      for (const auto& topic : diff.to_unsubscribe) {
        applied_.erase(topic);
        bindings_.erase(topic);
        last_subscribe_id_by_topic_.erase(topic);
      }
    }

    if (!diff.to_subscribe.empty()) {
      const std::string request_id = generateRequestId();
      socket_->sendText(buildSubscribeMessage(diff.to_subscribe, request_id));
      // Optimistically mark applied; a per-topic failure in the subscribe
      // response strips it back off (see handleSubscribeResponse), and it
      // retries on the next advertise delta rather than in a tight loop.
      // Remember the id so a STALE failure (from a subscribe this one
      // superseded) cannot erase applied_'s truth for the live subscription.
      for (const auto& topic : diff.to_subscribe) {
        applied_.insert(topic);
        last_subscribe_id_by_topic_[topic] = request_id;
      }
    }
  }

  // [poll thread] Apply the host's latest desired active-topic set, if it wrote
  // one since the last poll (latest-wins slot — see setActiveTopicsThunk).
  void applyDesiredTopics() {
    auto desired = desired_topics_slot_.take();
    if (!desired) {
      return;
    }
    last_applied_desired_ = std::move(*desired);
    reconcileSubscriptions();
  }

  // [poll thread] Dispatch one inbound text frame by SHAPE (not by request id —
  // the plugin never correlates ids): a subscribe response carries "schemas", a
  // topics_changed push carries "notification", a get_topics response carries a
  // "topics" array. Anything else is tolerated and ignored.
  void onTextMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    // The ONE hard compatibility gate: a response from a KNOWN-newer protocol
    // may carry semantics this client cannot interpret, so stop the stream
    // instead of guessing. Everything additive is feature-detected via the
    // `server` capability list instead (see handleGetTopicsResponse).
    if (const auto newer = unsupportedProtocolVersion(json, kSupportedProtocolVersion)) {
      const std::string upgrade = "server speaks protocol_version " + std::to_string(*newer) +
                                  " but this plugin supports " + std::to_string(kSupportedProtocolVersion) +
                                  " — upgrade PlotJuggler (or run an older plotjuggler_bridge)";
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, upgrade);
      fail(upgrade);
      return;  // do not interpret the frame
    }

    if (json.contains("notification")) {
      if (json["notification"] == "topics_changed") {
        handleTopicsChanged(json);
      }
      return;
    }

    // Any response carrying "schemas" is a subscribe response — INCLUDING the
    // all-failed shape (status "error" + ALL_SUBSCRIPTIONS_FAILED, schemas:{}),
    // whose failures must still strip applied_, or the rejected topics stay
    // "applied" forever and are never retried. Binding is gated on applied_
    // membership and failures on staleness, so processing is safe for every
    // status.
    if (json.contains("schemas")) {
      handleSubscribeResponse(json);
      return;
    }

    if (json.contains("topics") && json["topics"].is_array()) {
      handleGetTopicsResponse(json);
      return;
    }
  }

  // Subscribe response: bind a parser per schema entry (exactly as the original
  // plugin did — the host reuses bindings by signature, so re-binding an
  // unchanged topic is cheap) and strip any per-topic failures back off applied_.
  void handleSubscribeResponse(const nlohmann::json& json) {
    const auto& schemas = json["schemas"];
    if (schemas.is_object()) {
      for (auto it = schemas.begin(); it != schemas.end(); ++it) {
        const std::string& topic_name = it.key();
        // A LATE response for a topic the host has since paused must not
        // resurrect its binding — in-flight frames would ingest into an
        // inactive topic. (Legacy mode never populates applied_, so the gate
        // only applies in demand mode.)
        if (demand_mode_ && applied_.count(topic_name) == 0) {
          continue;
        }
        const auto& schema_obj = it.value();
        const std::string encoding = stringFieldOr(schema_obj, "encoding", "cdr");
        const std::string definition = stringField(schema_obj, "definition");
        const std::string schema_name = resolveSchemaName(topic_name);

        // Per-topic parser config, mirroring the foxglove source: parser_ros
        // needs schema_encoding to compile with the RIGHT schema language —
        // without it, it falls back to its ros2msg default, which happens to
        // work for the ROS2 backend but miscompiles the FastDDS backend's
        // omgidl schemas. use_embedded_timestamp is the key parser_ros reads
        // (use_timestamp kept for parsers using the older name).
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

    // Per-topic failures: the server forgets a failed subscribe (no sticky
    // retry), so drop it from applied_ — the next advertise delta re-proposes it.
    // Unless the failure is STALE (its request id predates the last subscribe we
    // sent for that topic): a newer subscribe superseded it, and erasing
    // applied_ would leave a live server-side subscription no pause can stop.
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

  // topics_changed push: apply the added/removed delta to the advertise catalog.
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
    // retypedTopics only inspects entries whose name is in applied_, so snapshot
    // just those — copying the whole catalog would clone every topic's multi-KB
    // schema definition to compare a handful of subscribed ones.
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

  // get_topics response: the authoritative FULL current topic set. Replace the
  // advertise catalog wholesale; a real change arms a re-advertise + reconcile.
  // Also the server-identity touchpoint: log who we're talking to ONCE and
  // warn (once) when a capability the demand UX relies on is missing.
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
      // Feature-detect by capability NAME; a pre-capability server (no
      // `server` object) is treated as having none. Never compare versions.
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

  // [host GUI thread] pj.topic_subscription.v1's set_active_topics: stash the
  // full desired set into desired_topics_slot_ and return. ctx is the
  // PjBridgeSource instance (DataSourceHandle::context()), per the ABI doc —
  // NOT the extension pointer returned by pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<PjBridgeSource*>(ctx);
    self->desired_topics_slot_.set(PJ::sdk::stringSetFromViews(names, count));
    return true;
  }

  PjBridgeDialog dialog_;
  BridgeConnectionOptions options_;

  std::string endpoint_;  ///< the ws:// or wss:// URL of this start, for connection and messages
  PJ::sdk::ArrayLimit array_limit_;
  bool use_timestamp_ = false;

  // Saved topic selection: the legacy subscribe list AND the demand-mode advertise
  // filter (see onStart). Keeps per-topic schema info for the legacy schema_name
  // lookup; the advertise filter is just its names, derived on demand.
  std::vector<TopicInfo> selected_topics_;

  std::unique_ptr<BridgeWebSocket> socket_;
  // Transport thread -> poll thread handoff of every inbound event, text and binary alike.
  // Fresh per start, so a late callback of a stopped session lands in a queue nobody drains.
  EventSink events_;
  std::map<std::string, PJ::ParserBindingHandle> bindings_;  // poll-thread-only

  ConnectionPhase phase_ = ConnectionPhase::kConnecting;
  Clock::time_point connect_deadline_{};
  Clock::time_point subscribe_deadline_{};

  std::vector<uint8_t> decompress_buffer_;
  int heartbeat_tick_ = 0;
  std::atomic<bool> paused_{false};  ///< written by the host's pause()/resume(), read on the poll thread
  // Poll-thread-only: every response is handled while draining events_.
  bool subscribe_response_received_ = false;

  // --- Demand-driven per-topic subscription (pj.topic_subscription.v1) ---

  // Decided once per start() by the first notify_available_topics attempt.
  // Poll-thread-only after start().
  bool demand_mode_ = false;

  // Set when handleTopicsChanged/handleGetTopicsResponse mutate the catalog, so
  // onPoll re-advertises + reconciles once per poll even across a burst of deltas.
  bool advertise_dirty_ = false;

  // The advertise catalog: topic name -> full info (type + encoding + schema).
  // Populated from get_topics / topics_changed. Poll-thread-only. Empty in legacy
  // mode (so resolveSchemaName falls back to selected_topics_).
  std::map<std::string, TopicInfo> advertised_topics_;

  // Topics with a live subscription right now. Poll-thread-only. Grown
  // optimistically at subscribe, shrunk by unsubscribe and per-topic failures.
  std::set<std::string> applied_;

  // The first stream-stopping failure (connect timeout, close, error, or a
  // response from a KNOWN-newer protocol); onPoll returns it until stop() clears it.
  std::string fatal_error_;
  // One server-identity log + capability warning per start.
  bool server_info_logged_ = false;
  // Data frames already reported as dropped, and when; see reportDroppedData().
  uint64_t reported_dropped_frames_ = 0;
  Clock::time_point last_drop_warning_{};

  // The id of the LAST subscribe request naming each topic — lets a stale
  // per-topic failure (from a superseded request) be ignored instead of
  // erasing applied_'s truth. Pruned on unsubscribe/retype; cleared in onStop.
  std::map<std::string, std::string> last_subscribe_id_by_topic_;

  // Most recent desired-topic set from the host (via applyDesiredTopics).
  // Poll-thread-only. Retained so a topic advertised AFTER the host asked for it
  // gets subscribed without waiting for another set_active_topics.
  std::set<std::string> last_applied_desired_;

  // Cross-thread control-plane surface: setActiveTopicsThunk (host GUI thread)
  // writes; applyDesiredTopics (poll thread) drains. Its own mutex — the only
  // cross-thread surface besides events_.
  DesiredTopicsSlot desired_topics_slot_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(PjBridgeSource, kPjBridgeManifest)

PJ_DIALOG_PLUGIN(PjBridgeDialog, kPjBridgeManifest)
