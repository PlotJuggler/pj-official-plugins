#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_array_policy/array_policy.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base64/base64.hpp>
#include <pj_streaming/drain_queue.hpp>
#include <pj_streaming/endpoint.hpp>
#include <pj_streaming/websocket_utils.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "foxglove_dialog.hpp"
#include "foxglove_manifest.hpp"
#include "foxglove_protocol.hpp"
#include "foxglove_topic_subscription.hpp"

namespace {

using namespace PJ::FoxgloveProtocol;

struct QueuedBinaryMessage {
  uint32_t subscription_id;
  int64_t timestamp_ns;
  std::vector<uint8_t> payload;
};

struct QueuedTextMessage {
  std::string text;
};

class FoxgloveSource : public PJ::StreamSourceBase {
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

    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 8765);
    array_limit_ = pj::array_policy::arrayLimitFromJson(cfg);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Read selected channels with schema info from dialog config
    selected_channels_.clear();
    if (cfg.contains("channels") && cfg["channels"].is_array()) {
      for (const auto& ch : cfg["channels"]) {
        ChannelInfo info;
        info.id = ch.value("id", uint64_t{0});
        info.topic = ch.value("topic", "");
        info.encoding = ch.value("encoding", "");
        info.schema_name = ch.value("schema_name", "");
        info.schema = ch.value("schema", "");
        info.schema_encoding = ch.value("schema_encoding", "");
        if (!info.topic.empty()) {
          selected_channels_.push_back(std::move(info));
        }
      }
    }

    // Steal the live socket from the dialog (it stays connected on accept).
    socket_ = dialog_.takeSocket();

    // Message callback for the streaming source. Text messages (advertise/
    // unadvertise) are queued and processed in onPoll() so that
    // ensureParserBinding() is always called from the poll thread.
    const auto install_message_callback = [this](ix::WebSocket& socket) {
      socket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
          if (msg->binary) {
            onBinaryMessage(msg->str);
          } else {
            text_queue_.push({msg->str});
          }
        } else if (msg->type == ix::WebSocketMessageType::Close) {
          onDisconnected();
        }
      });
    };

    if (!socket_ || socket_->getReadyState() != ix::ReadyState::Open) {
      // Fallback: connect fresh (e.g. when started without dialog via saved config)
      socket_ = std::make_unique<ix::WebSocket>();
      socket_->setUrl(pj::streaming::composeEndpoint("ws", address_, static_cast<uint16_t>(port_)));
      socket_->addSubProtocol("foxglove.sdk.v1");
      socket_->disableAutomaticReconnection();
      // Install BEFORE start(): the server sends its one-shot serverInfo +
      // "advertise" burst the moment the connection opens, and a callback-less
      // socket drops frames — the burst must land in text_queue_, or demand mode
      // starts with an empty channel catalog it can never refill (the server
      // does not re-advertise on request).
      install_message_callback(*socket_);
      if (!pj::streaming::startAndWaitForOpen(*socket_, std::chrono::seconds(5))) {
        socket_->stop();
        return PJ::unexpected(
            "failed to connect to Foxglove bridge at " +
            pj::streaming::composeHostPort(address_, static_cast<uint16_t>(port_)));
      }
    } else {
      // Stolen socket: re-register for the streaming source (the dialog's
      // callback goes away with it). The initial advertise burst already
      // happened — the dialog's discovered catalog is seeded below instead.
      install_message_callback(*socket_);
    }

    // When the socket is stolen from the dialog it is already connected and the server
    // already sent its initial "advertise" burst — it will not re-send it. The dialog
    // discovered the FULL channel catalog during that burst (not just what the user
    // selected); seed our view of it now, since this is the only way we learn about
    // unselected topics on the stolen-socket path.
    for (const auto& ch : dialog_.allDiscoveredChannels()) {
      advertised_channels_[ch.id] = ch;
    }

    // Demand mode is decided ONCE per start, by the very first advertise attempt: a
    // host that lacks the notify_available_topics tail slot (old PJ4) returns an
    // explicit error, so a new plugin detects it and degrades to the legacy
    // selection-driven subscribe path below, byte-for-byte. A host that accepts it is
    // re-notified on every advertise/unadvertise delta (see onTextMessage) and drives
    // subscriptions solely through pj.topic_subscription.v1 (see onPoll / applyDesiredTopics).
    demand_mode_ = static_cast<bool>(refreshAdvertisedTopics());

    // Demand mode has no auto-subscribe: the host asks for exactly what it wants to
    // display via set_active_topics, applied on the poll thread (see onPoll).
    if (!demand_mode_) {
      if (selected_channels_.empty()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "No channels selected — connected to a legacy PlotJuggler host, which cannot "
            "advertise topics on demand. Nothing will stream until the connection dialog is "
            "reopened and channels are selected.");
      }
      // Subscribe directly using the channel info captured by the dialog.
      subscribeToSelectedChannels();
    }

    connected_ = true;
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Non-blocking reconnection: if connection was lost, try every ~5s.
    // MUST run before the text drain below: the new connection's one-shot
    // serverInfo+advertise burst may already sit in text_queue_ when reconnect
    // success is detected, and the state reset here has to happen BEFORE that
    // advertise is processed — draining first would populate the catalog and
    // then immediately wipe it, losing the burst for good (the server does not
    // re-advertise).
    if (!connected_ && socket_) {
      if (reconnect_pending_) {
        // Check if the async reconnect succeeded
        if (socket_->getReadyState() == ix::ReadyState::Open) {
          connected_ = true;
          reconnect_pending_ = false;
          reconnect_tick_ = 0;
          // Reset subscription state so new advertise messages create fresh bindings
          subscriptions_.clear();
          binding_by_subscription_.clear();
          advertised_channels_.clear();
          next_subscription_id_ = 1;
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Reconnected to Foxglove bridge");
        } else if (socket_->getReadyState() == ix::ReadyState::Closed) {
          // Connection attempt failed
          reconnect_pending_ = false;
        }
      } else if (++reconnect_tick_ >= 150) {
        reconnect_tick_ = 0;
        reconnect_pending_ = true;
        socket_->stop();
        socket_->start();  // async — result checked on next poll
      }
    }

    // Drain text messages (advertise/unadvertise) queued from the WebSocket callback thread.
    // ensureParserBinding() must be called from the poll thread, not the callback thread.
    auto text_batch = text_queue_.drain();
    while (!text_batch.empty()) {
      onTextMessage(text_batch.front().text);
      text_batch.pop();
    }

    // Demand mode: apply the host's latest desired active-topic set, if it wrote one
    // since the last poll. demand_mode_ and last_applied_desired_ deliberately survive
    // a reconnect above (host capability doesn't change mid-session) — the channels
    // that just got cleared will re-subscribe against last_applied_desired_ as they
    // re-advertise (reconcileSubscriptions runs after every advertise delta), so no
    // special reconnect handling is needed here.
    if (demand_mode_ && connected_) {
      applyDesiredTopics();
    }

    auto batch = message_queue_.drain();

    while (!batch.empty()) {
      auto& msg = batch.front();

      auto it = binding_by_subscription_.find(msg.subscription_id);
      if (it != binding_by_subscription_.end()) {
        // Move the per-message payload into a shared_ptr-owned buffer so the
        // PayloadView fetcher remains valid after onPoll returns
        // (ObjectIngestPolicy may defer dispatch beyond this call).
        auto payload = std::make_shared<std::vector<uint8_t>>(std::move(msg.payload));
        auto status = runtimeHost().pushMessage(
            it->second, PJ::Timestamp{msg.timestamp_ns},
            [payload]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{payload}; });
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning, "Failed to push message: " + status.error());
        }
      }

      batch.pop();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (socket_) {
      if (connected_ && !subscriptions_.empty()) {
        std::vector<uint32_t> ids;
        for (const auto& [id, _channel_id] : subscriptions_) {
          ids.push_back(id);
        }
        socket_->sendText(buildUnsubscribeMessage(ids));
      }

      socket_->stop();
      socket_.reset();
    }
    connected_ = false;
    selected_channels_.clear();
    subscriptions_.clear();
    binding_by_subscription_.clear();
    advertised_channels_.clear();
    next_subscription_id_ = 1;
    demand_mode_ = false;
    last_applied_desired_.clear();
    (void)desired_topics_slot_.take();  // drop any pending host command so it can't leak into the next start
  }

 private:
  // Resolve a channel's schema payload once, in one place, for both binding
  // (subscribe) and advertising: Foxglove base64-encodes binary schemas (the
  // protobuf FileDescriptorSet) in the advertise JSON, while text schemas
  // (ros2msg/omgidl) arrive verbatim. Zero-copy for text schemas (the view aliases
  // ch.schema); base64 decodes into `b64_storage`, which must outlive the returned
  // view (unused otherwise).
  static std::string_view resolveSchemaBytes(
      const ChannelInfo& ch, const ChannelRoute& route, std::string& b64_storage) {
    if (route.schema_is_base64) {
      b64_storage = PJ::base64::decode(ch.schema);
      return b64_storage;
    }
    return ch.schema;
  }

  // Create a parser binding for a supported channel and record it for its
  // subscription id. Returns an error string on binding failure, std::nullopt
  // on success. Shared by both subscribe paths (stolen-socket and fresh-connect).
  std::optional<std::string> bindChannel(const ChannelInfo& ch, uint32_t sub_id) {
    const ChannelRoute route = classifyChannel(ch);

    nlohmann::json parser_cfg;
    pj::array_policy::arrayLimitToJson(parser_cfg, array_limit_);
    parser_cfg["use_timestamp"] = use_timestamp_;
    parser_cfg["use_embedded_timestamp"] = use_timestamp_;
    parser_cfg["schema_encoding"] = route.parser_encoding;

    // `decoded` must outlive the ensureParserBinding call below — the host
    // consumes the schema span synchronously, same as data_load_mcap.
    std::string decoded;
    const std::string_view schema_bytes = resolveSchemaBytes(ch, route, decoded);
    const PJ::Span<const uint8_t> schema_span(
        reinterpret_cast<const uint8_t*>(schema_bytes.data()), schema_bytes.size());

    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = ch.topic,
        .parser_encoding = route.parser_encoding,
        .type_name = ch.schema_name,
        .schema = schema_span,
        .parser_config_json = parser_cfg.dump(),
    });
    if (!binding) {
      return ch.topic + " (" + route.parser_encoding + "): " + binding.error();
    }
    binding_by_subscription_[sub_id] = *binding;
    return std::nullopt;
  }

  void reportParserErrors(const std::vector<std::string>& parser_errors) {
    if (parser_errors.empty()) {
      return;
    }
    std::string msg = "Skipped " + std::to_string(parser_errors.size()) + " channel(s):\n";
    for (const auto& e : parser_errors) {
      msg += "  - " + e + "\n";
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, msg);
  }

  // Allocate a subscription id for `ch`, record it, and bind its parser; binding
  // failures land in `parser_errors`. The caller sends the subscribe frame (the
  // legacy paths send per-channel, reconcileSubscriptions batches).
  uint32_t registerSubscription(const ChannelInfo& ch, std::vector<std::string>& parser_errors) {
    const uint32_t sub_id = next_subscription_id_++;
    subscriptions_[sub_id] = ch.id;
    if (auto err = bindChannel(ch, sub_id)) {
      parser_errors.push_back(*err);
    }
    return sub_id;
  }

  // Build the AvailableTopic list for notify_available_topics from advertised_channels_:
  // supported encodings only, restricted by the dialog's saved selection. That selection
  // is dual-purpose (see foxglove_dialog.hpp's OK-gate comment): on a legacy host it is
  // the subscribe list (subscribeToSelectedChannels); here, in demand mode, it acts as an
  // OPTIONAL advertise filter — an empty selection means "advertise everything".
  // `schema_storage` backs the base64-decoded schema spans and must outlive the returned
  // topics; it is reserved up front so no push_back can reallocate out from under an
  // already-taken span (text-schema spans alias advertised_channels_ directly).
  std::vector<PJ::AvailableTopic> buildAvailableTopics(std::vector<std::string>& schema_storage) const {
    schema_storage.clear();
    schema_storage.reserve(advertised_channels_.size());
    std::vector<PJ::AvailableTopic> topics;
    topics.reserve(advertised_channels_.size());

    for (const auto& [_id, ch] : advertised_channels_) {
      if (!passesAdvertiseFilter(ch.topic, selected_channels_)) {
        continue;
      }
      const ChannelRoute route = classifyChannel(ch);
      if (!route.supported) {
        continue;
      }
      const std::string_view bytes = resolveSchemaBytes(ch, route, schema_storage.emplace_back());
      topics.push_back(
          PJ::AvailableTopic{
              ch.topic, route.parser_encoding, ch.schema_name,
              PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())});
    }
    return topics;
  }

  // Advertise the current (filtered) full channel set to the host. The FIRST call, from
  // onStart(), also decides demand_mode_ via the returned status. Later calls (from
  // onTextMessage, after an advertise/unadvertise delta) are only made when demand_mode_ is
  // already true — no point re-probing a host that has already been found to lack the slot.
  PJ::Status refreshAdvertisedTopics() {
    std::vector<std::string> schema_storage;
    const auto topics = buildAvailableTopics(schema_storage);
    return runtimeHost().notifyAvailableTopics(PJ::Span<const PJ::AvailableTopic>(topics.data(), topics.size()));
  }

  // Demand-mode wrapper around refreshAdvertisedTopics() for the steady-state calls
  // (advertise/unadvertise deltas), where a failure is only worth a warning — unlike
  // the first call in onStart(), whose status DECIDES demand_mode_.
  void refreshAdvertisedTopicsAndWarn() {
    if (auto status = refreshAdvertisedTopics(); !status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "Failed to advertise topics: " + status.error());
    }
  }

  // [poll thread] The single reconciliation point for demand mode: diff the live
  // subscriptions against last_applied_desired_ over the currently-advertised channels,
  // unsubscribing what fell out and subscribing+binding what is newly wanted. Called
  // from applyDesiredTopics (a new desired set landed) AND after every advertise delta
  // in onTextMessage — so desired-then-advertised (mid-stream advertise, or the
  // resubscribe burst after a reconnect) and advertised-then-desired converge on the
  // same code path instead of two.
  void reconcileSubscriptions() {
    // Filter-aware: a topic the advertise filter hides from the host must not be
    // subscribed either (see filteredAdvertisedTopics) — and a live subscription
    // to a now-filtered topic is dropped by the same diff.
    const auto advertised_topics = filteredAdvertisedTopics(advertised_channels_, selected_channels_);

    const auto diff = computeSubscriptionDiff(subscriptions_, advertised_topics, last_applied_desired_);

    if (!diff.to_unsubscribe.empty()) {
      socket_->sendText(buildUnsubscribeMessage(diff.to_unsubscribe));
      for (uint32_t sub_id : diff.to_unsubscribe) {
        subscriptions_.erase(sub_id);
        binding_by_subscription_.erase(sub_id);
      }
    }

    if (!diff.to_subscribe_channel_ids.empty()) {
      std::vector<std::pair<uint32_t, uint64_t>> to_send;
      std::vector<std::string> parser_errors;
      for (uint64_t channel_id : diff.to_subscribe_channel_ids) {
        auto it = advertised_channels_.find(channel_id);
        if (it == advertised_channels_.end()) {
          continue;
        }
        to_send.emplace_back(registerSubscription(it->second, parser_errors), channel_id);
      }
      if (!to_send.empty()) {
        socket_->sendText(buildSubscribeMessage(to_send));
      }
      reportParserErrors(parser_errors);
    }
  }

  // [poll thread] Apply the host's latest desired active-topic set, if it wrote one
  // since the last poll (latest-wins slot — see setActiveTopicsThunk).
  void applyDesiredTopics() {
    auto desired = desired_topics_slot_.take();
    if (!desired) {
      return;
    }
    last_applied_desired_ = std::move(*desired);
    reconcileSubscriptions();
  }

  // Subscribe to all selected channels using the channel info already captured by the dialog.
  // Called from onStart() when the socket is stolen (server won't re-send "advertise").
  // Also safe to call after a fresh connect where selected_channels_ is pre-populated.
  void subscribeToSelectedChannels() {
    std::vector<std::string> parser_errors;

    for (const auto& ch : selected_channels_) {
      if (!classifyChannel(ch).supported) {
        parser_errors.push_back(ch.topic + " (" + ch.schema_encoding + "): unsupported encoding");
        continue;
      }

      advertised_channels_[ch.id] = ch;

      const uint32_t sub_id = registerSubscription(ch, parser_errors);
      socket_->sendText(buildSubscribeMessage({{sub_id, ch.id}}));
    }

    reportParserErrors(parser_errors);
  }

  void onTextMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    std::string op = json.value("op", "");

    if (op == "advertise") {
      auto channels_arr = json.value("channels", nlohmann::json::array());
      std::vector<std::string> parser_errors;

      for (const auto& ch_json : channels_arr) {
        ChannelInfo ch;
        ch.id = ch_json.value("id", uint64_t{0});
        ch.topic = ch_json.value("topic", "");
        ch.encoding = ch_json.value("encoding", "");
        ch.schema_name = ch_json.value("schemaName", "");
        ch.schema = ch_json.value("schema", "");
        ch.schema_encoding = ch_json.value("schemaEncoding", "");

        // Track server-advertised channels (full info — schema included — so
        // refreshAdvertisedTopics()/bindChannel() never need to re-parse).
        advertised_channels_[ch.id] = ch;

        if (demand_mode_) {
          continue;  // subscriptions are reconciled once, below, against the host's desired set
        }

        // Legacy mode: the dialog's saved selection is the subscribe list, exactly
        // as before pj.topic_subscription.v1 existed.
        bool user_selected = false;
        for (const auto& sel : selected_channels_) {
          if (sel.topic == ch.topic) {
            user_selected = true;
            break;
          }
        }
        if (!user_selected) {
          continue;
        }
        if (!classifyChannel(ch).supported) {
          parser_errors.push_back(ch.topic + " (" + ch.schema_encoding + "): unsupported encoding");
          continue;
        }

        const uint32_t sub_id = registerSubscription(ch, parser_errors);
        socket_->sendText(buildSubscribeMessage({{sub_id, ch.id}}));
      }

      reportParserErrors(parser_errors);

      if (demand_mode_ && !channels_arr.empty()) {
        // A channel may advertise AFTER the host already asked for its topic (mid-stream
        // advertise, or the resubscribe burst after a reconnect) — the reconcile picks it
        // up from last_applied_desired_ without waiting for another set_active_topics call.
        reconcileSubscriptions();
        refreshAdvertisedTopicsAndWarn();
      }
    }

    // Handle unadvertise: server removed channels
    if (op == "unadvertise") {
      auto channel_ids = json.value("channelIds", nlohmann::json::array());
      std::vector<std::string> removed_topics;
      for (const auto& id_json : channel_ids) {
        uint64_t removed_id = id_json.get<uint64_t>();
        auto it = advertised_channels_.find(removed_id);
        if (it != advertised_channels_.end()) {
          removed_topics.push_back(it->second.topic);
          advertised_channels_.erase(it);
        }
        // Clean up subscriptions referencing removed channels
        for (auto sub_it = subscriptions_.begin(); sub_it != subscriptions_.end();) {
          if (sub_it->second == removed_id) {
            binding_by_subscription_.erase(sub_it->first);
            sub_it = subscriptions_.erase(sub_it);
          } else {
            ++sub_it;
          }
        }
      }
      if (!removed_topics.empty()) {
        std::string msg = "Server removed " + std::to_string(removed_topics.size()) + " channel(s):";
        for (const auto& t : removed_topics) {
          msg += " " + t;
        }
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, msg);

        if (demand_mode_) {
          refreshAdvertisedTopicsAndWarn();
        }
      }
    }

    // Handle server status/warning messages (level: 0=info, 1=warning, 2=error)
    if (op == "status") {
      int level = json.value("level", 0);
      std::string status_msg = json.value("message", "");
      auto pj_level = PJ::DataSourceMessageLevel::kInfo;
      if (level == 1) {
        pj_level = PJ::DataSourceMessageLevel::kWarning;
      }
      if (level >= 2) {
        pj_level = PJ::DataSourceMessageLevel::kError;
      }
      runtimeHost().reportMessage(pj_level, "Foxglove server: " + status_msg);
    }
  }

  void onDisconnected() {
    if (connected_) {
      connected_ = false;
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, "Foxglove bridge connection lost");
    }
  }

  void onBinaryMessage(const std::string& message) {
    BinaryFrame frame;
    if (!parseBinaryFrame(reinterpret_cast<const uint8_t*>(message.data()), message.size(), frame)) {
      return;
    }

    QueuedBinaryMessage msg;
    msg.subscription_id = frame.subscription_id;
    msg.timestamp_ns = static_cast<int64_t>(frame.log_time_ns);
    msg.payload.assign(frame.payload_data, frame.payload_data + frame.payload_size);

    message_queue_.push(std::move(msg));
  }

  FoxgloveDialog dialog_;

  std::string address_ = "localhost";
  int port_ = 8765;
  pj::array_policy::ArrayLimit array_limit_;
  bool use_timestamp_ = false;

  std::vector<ChannelInfo> selected_channels_;
  std::unique_ptr<ix::WebSocket> socket_;
  std::atomic<bool> connected_ = false;

  std::map<uint32_t, uint64_t> subscriptions_;  // sub_id -> channel_id
  std::map<uint32_t, PJ::ParserBindingHandle> binding_by_subscription_;
  std::map<uint64_t, ChannelInfo> advertised_channels_;  // channel_id -> full channel info (schema included)
  uint32_t next_subscription_id_ = 1;

  pj::streaming::DrainQueue<QueuedBinaryMessage> message_queue_;
  pj::streaming::DrainQueue<QueuedTextMessage> text_queue_;
  int reconnect_tick_ = 0;
  std::atomic<bool> reconnect_pending_ = false;

  // --- Demand-driven per-topic subscription (pj.topic_subscription.v1) ---

  // Decided once per start() by the first notify_available_topics attempt (see onStart);
  // false means an old host was detected and the legacy selection-driven subscribe path is
  // used instead, byte-for-byte. Poll-thread-only after start().
  bool demand_mode_ = false;

  // The most recent desired-topic set delivered by the host (via applyDesiredTopics).
  // Poll-thread-only. Retained so reconcileSubscriptions() can pick up a channel that
  // advertises AFTER the host already asked for its topic (mid-stream advertise, or the
  // resubscribe burst after a reconnect).
  std::set<std::string> last_applied_desired_;

  // Cross-thread control-plane surface: setActiveTopicsThunk (entered on the HOST GUI thread via
  // pluginExtension/set_active_topics) writes; applyDesiredTopics (poll thread, from onPoll) drains.
  // Its own mutex — the only new cross-thread surface this feature adds; subscriptions_/
  // advertised_channels_/binding_by_subscription_ stay poll-thread-only, same as before.
  PJ::FoxgloveProtocol::DesiredTopicsSlot desired_topics_slot_;

  // [host GUI thread] pj.topic_subscription.v1's set_active_topics: stash the full desired set
  // into desired_topics_slot_ and return. ctx is the FoxgloveSource instance (DataSourceHandle::
  // context()), per the ABI doc — NOT the extension pointer returned by pluginExtension().
  static bool setActiveTopicsThunk(
      void* ctx, const PJ_string_view_t* names, uint64_t count, PJ_error_t* /*out_error*/) noexcept {
    auto* self = static_cast<FoxgloveSource*>(ctx);
    self->desired_topics_slot_.set(pj::streaming::stringSetFromViews(names, count));
    return true;
  }
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(FoxgloveSource, kFoxgloveManifest)

PJ_DIALOG_PLUGIN(FoxgloveDialog)
