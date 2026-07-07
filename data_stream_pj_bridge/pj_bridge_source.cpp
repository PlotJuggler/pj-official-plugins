#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_array_policy/array_policy.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pj_bridge_dialog.hpp"
#include "pj_bridge_manifest.hpp"
#include "pj_bridge_protocol.hpp"
#include "pj_bridge_topic_subscription.hpp"

namespace {

using namespace PJ::BridgeProtocol;

struct QueuedFrame {
  std::vector<uint8_t> data;
};

class PjBridgeSource : public PJ::StreamSourceBase {
 public:
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

  PJ::Status pause() override {
    if (socket_ && socket_->getReadyState() == ix::ReadyState::Open) {
      socket_->sendText(buildRequest("pause", generateRequestId()));
      paused_ = true;
    }
    return PJ::okStatus();
  }

  PJ::Status resume() override {
    if (socket_ && socket_->getReadyState() == ix::ReadyState::Open) {
      socket_->sendText(buildRequest("resume", generateRequestId()));
      paused_ = false;
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }

    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = cfg.value("port", 9871);
    array_limit_ = pj::array_policy::arrayLimitFromJson(cfg);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Read the saved topic selection. On a legacy host this is the subscribe
    // set (exactly as before); on a demand-capable host it is only an OPTIONAL
    // advertise filter (empty = advertise everything).
    selected_topics_.clear();
    advertise_filter_.clear();
    if (cfg.contains("topics") && cfg["topics"].is_array()) {
      for (const auto& t : cfg["topics"]) {
        TopicInfo info;
        info.name = t.value("name", "");
        info.encoding = t.value("schema_encoding", "cdr");
        info.schema_name = t.value("schema_name", "");
        info.schema = t.value("schema_definition", "");
        if (!info.name.empty()) {
          advertise_filter_.push_back(info.name);
          selected_topics_.push_back(std::move(info));
        }
      }
    }

    // Steal the live socket from the dialog (it stays connected on accept).
    // This mirrors the original plugin where one socket serves both dialog and streaming.
    socket_ = dialog_.takeSocket();

    if (!socket_ || socket_->getReadyState() != ix::ReadyState::Open) {
      // Fallback: connect fresh (e.g. when started without dialog via saved config)
      socket_ = std::make_unique<ix::WebSocket>();
      socket_->setUrl("ws://" + address_ + ":" + std::to_string(port_));
      socket_->disableAutomaticReconnection();
      socket_->start();

      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        auto state = socket_->getReadyState();
        if (state == ix::ReadyState::Open) {
          break;
        }
        if (state == ix::ReadyState::Closed) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      if (socket_->getReadyState() != ix::ReadyState::Open) {
        socket_->stop();
        return PJ::unexpected(
            std::string("failed to connect to PJ bridge at ") + address_ + ":" + std::to_string(port_));
      }
    } else {
      // Stolen socket: seed the advertise catalog from the topics the dialog
      // already discovered, so demand mode can list them immediately instead of
      // waiting for our own get_topics round-trip below. Discarded if the host
      // turns out to be legacy.
      for (auto& info : dialog_.allDiscoveredTopics()) {
        advertised_topics_[info.name] = std::move(info);
      }
    }

    // Re-register the message callback for the streaming source. TEXT frames are
    // QUEUED and processed on the poll thread (see pumpTextQueue): subscribe now
    // happens mid-stream, and parser bindings (bindings_) must only ever be
    // created/read on the poll thread — never on this socket callback thread.
    socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Message) {
        if (msg->binary) {
          onBinaryMessage(msg->str);
        } else {
          text_frames_.push(msg->str);
        }
      }
    });

    // Demand mode is decided ONCE per start by the first notify_available_topics
    // attempt: a host that lacks the tail slot (old PJ4) returns an error, so a
    // new plugin degrades to the legacy selection-driven subscribe path below,
    // byte-for-byte. A host that accepts it drives subscriptions solely through
    // pj.topic_subscription.v1 (see onPoll / applyDesiredTopics).
    demand_mode_ = static_cast<bool>(refreshAdvertisedTopics());

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
      return PJ::okStatus();
    }

    // --- Legacy host: preserve the original behavior exactly. ---
    // The dialog seed is only for demand mode; drop it so the legacy schema
    // lookup below resolves from selected_topics_ alone.
    advertised_topics_.clear();
    if (selected_topics_.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "No topics selected — connected to a legacy PlotJuggler host, which cannot advertise "
          "topics on demand. Nothing will stream until the connection dialog is reopened and "
          "topics are selected.");
    }

    // Subscribe to the selected topics — the response carries the schemas needed for parsing.
    subscribe_response_received_ = false;
    std::vector<std::string> names;
    names.reserve(selected_topics_.size());
    for (const auto& info : selected_topics_) {
      names.push_back(info.name);
    }
    socket_->sendText(buildSubscribeMessage(names, generateRequestId()));

    // Wait for the subscribe response (contains schemas). Pump the text queue
    // ourselves while waiting — the response now lands in text_frames_, and
    // onPoll is not running yet, so nothing else would drain it.
    auto sub_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!subscribe_response_received_ && std::chrono::steady_clock::now() < sub_deadline) {
      pumpTextQueue();
      if (subscribe_response_received_) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!subscribe_response_received_) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Subscribe response not received; parsers may lack schemas");
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Heartbeat: send every ~1s (30 polls at 33ms each)
    if (socket_ && ++heartbeat_tick_ >= 30) {
      heartbeat_tick_ = 0;
      socket_->sendText(buildRequest("heartbeat", generateRequestId()));
    }

    // Drain inbound text frames (subscribe responses / topics_changed / get_topics)
    // on the poll thread so all parser-binding work stays off the socket thread.
    pumpTextQueue();

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

    // Process queued binary frames
    std::queue<QueuedFrame> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, frame_queue_);
    }

    while (!batch.empty()) {
      auto& frame = batch.front();

      std::vector<RawMessage> messages;
      if (parseBinaryFrame(frame.data.data(), frame.data.size(), messages, decompress_buffer_)) {
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

      batch.pop();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (socket_) {
      socket_->stop();
      socket_.reset();
    }
    bindings_.clear();
    selected_topics_.clear();
    advertise_filter_.clear();
    advertised_topics_.clear();
    applied_.clear();
    last_subscribe_id_by_topic_.clear();
    last_applied_desired_.clear();
    demand_mode_ = false;
    advertise_dirty_ = false;
    (void)text_frames_.drain();         // drop any un-processed inbound frames
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

 private:
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

  // Build the AvailableTopic list for notify_available_topics from the advertise
  // catalog, restricted by the (optional) advertise filter. Schema bytes alias
  // the TopicInfo::schema strings held in advertised_topics_, which outlive the
  // synchronously-consumed notify_available_topics call. Mirrors the ros2 /
  // foxglove parameter shape: {topic, parser_encoding, type_name, schema}.
  std::vector<PJ::AvailableTopic> buildAvailableTopics() const {
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(advertised_topics_.size());
    for (const auto& [name, info] : advertised_topics_) {
      if (!passesAdvertiseFilter(name, advertise_filter_)) {
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
    const auto advertised = filteredAdvertisedTopics(advertised_topics_, advertise_filter_);
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

  // [poll thread] Drain and process every queued inbound text frame in order.
  // A frame that throws (wire data can be arbitrarily malformed — nlohmann
  // accessors throw on wrong-typed fields) is dropped with a warning; it must
  // never fail the host's poll.
  void pumpTextQueue() {
    auto batch = text_frames_.drain();
    while (!batch.empty()) {
      try {
        onTextMessage(batch.front());
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, std::string("Dropped malformed control frame: ") + e.what());
      }
      batch.pop();
    }
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
        pj::array_policy::arrayLimitToJson(parser_cfg, array_limit_);
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
    const auto before = advertised_topics_;
    if (applyAdvertiseDelta(advertised_topics_, added, removed)) {
      dropRetypedApplied(before);
      advertise_dirty_ = true;
    }
  }

  // get_topics response: the authoritative FULL current topic set. Replace the
  // advertise catalog wholesale; a real change arms a re-advertise + reconcile.
  void handleGetTopicsResponse(const nlohmann::json& json) {
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

  void onBinaryMessage(const std::string& message) {
    QueuedFrame frame;
    frame.data.assign(
        reinterpret_cast<const uint8_t*>(message.data()),
        reinterpret_cast<const uint8_t*>(message.data()) + message.size());
    std::lock_guard<std::mutex> lock(queue_mutex_);
    frame_queue_.push(std::move(frame));
  }

  // [host GUI thread] pj.topic_subscription.v1's set_active_topics: stash the
  // full desired set into desired_topics_slot_ and return. ctx is the
  // PjBridgeSource instance (DataSourceHandle::context()), per the ABI doc —
  // NOT the extension pointer returned by pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<PjBridgeSource*>(ctx);
    std::set<std::string> topics;
    for (uint64_t i = 0; i < count; ++i) {
      topics.emplace(names[i].data, names[i].size);
    }
    self->desired_topics_slot_.set(std::move(topics));
    return true;
  }

  PjBridgeDialog dialog_;

  std::string address_ = "127.0.0.1";
  int port_ = 9871;
  pj::array_policy::ArrayLimit array_limit_;
  bool use_timestamp_ = false;

  // Saved topic selection: the legacy subscribe list AND the demand-mode advertise
  // filter (see onStart). selected_topics_ keeps per-topic schema info for the
  // legacy schema_name lookup; advertise_filter_ is just the names.
  std::vector<TopicInfo> selected_topics_;
  std::vector<std::string> advertise_filter_;

  std::unique_ptr<ix::WebSocket> socket_;
  std::map<std::string, PJ::ParserBindingHandle> bindings_;  // poll-thread-only

  std::mutex queue_mutex_;
  std::queue<QueuedFrame> frame_queue_;
  std::vector<uint8_t> decompress_buffer_;
  int heartbeat_tick_ = 0;
  bool paused_ = false;
  std::atomic<bool> subscribe_response_received_ = false;

  // --- Demand-driven per-topic subscription (pj.topic_subscription.v1) ---

  // Socket-thread → poll-thread handoff for inbound TEXT frames. The race fix:
  // bindings_ is only ever touched by the poll thread draining this queue.
  TextFrameQueue text_frames_;

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

  // The id of the LAST subscribe request naming each topic — lets a stale
  // per-topic failure (from a superseded request) be ignored instead of
  // erasing applied_'s truth. Pruned on unsubscribe/retype; cleared in onStop.
  std::map<std::string, std::string> last_subscribe_id_by_topic_;

  // Most recent desired-topic set from the host (via applyDesiredTopics).
  // Poll-thread-only. Retained so a topic advertised AFTER the host asked for it
  // gets subscribed without waiting for another set_active_topics.
  std::set<std::string> last_applied_desired_;

  // Cross-thread control-plane surface: setActiveTopicsThunk (host GUI thread)
  // writes; applyDesiredTopics (poll thread) drains. Its own mutex — the only new
  // cross-thread surface besides text_frames_/frame_queue_.
  DesiredTopicsSlot desired_topics_slot_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(PjBridgeSource, kPjBridgeManifest)

PJ_DIALOG_PLUGIN(PjBridgeDialog)
