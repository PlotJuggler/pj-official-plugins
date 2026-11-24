#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base64/base64.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <pj_websocket/source_connection.hpp>
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

/// Deadlines the source enforces by polling; tests shorten them.
struct FoxgloveConnectionOptions : PJ::WebSocket::DialogConnectionOptions {
  /// Wait before each reconnection attempt after the connection was lost.
  std::chrono::milliseconds reconnect{5000};
};

class FoxgloveSource : public PJ::StreamSourceBase {
 public:
  explicit FoxgloveSource(FoxgloveConnectionOptions options = {})
      : dialog_(options),
        connection_(
            {.name = "Foxglove bridge",
             .connect = options.connect,
             .reconnect = options.reconnect,
             .inbound_queue_bytes = options.inbound_queue_bytes},
            {[this](bool reconnected) { onOpen(reconnected); },
             [this](const std::string& text) { onTextMessage(text); },
             [this](std::string&& frame) { onBinaryMessage(std::move(frame)); },
             [this](PJ::DataSourceMessageLevel level, const std::string& message) {
               runtimeHost().reportMessage(level, message);
             }}) {}

  /// The default services, plus the optional settings store the dialog remembers its last URL in.
  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = StreamSourceBase::bind(services);
    dialog_.setSettings(services.get<PJ::sdk::SettingsStoreService>().value_or(PJ::sdk::SettingsView{}));
    return status;
  }

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

    // Adopt the dialog's connection when it hands over an open one (it stays connected on
    // accept); otherwise connect fresh, e.g. for a configured start without the dialog. Either
    // way start() returns before the connection completes: onPoll() drives the rest, and every
    // transport event (the server's one-shot serverInfo + "advertise" burst included) waits in
    // the connection's queue in arrival order until the poll thread applies it.
    adopted_ = connection_.adopt(dialog_.takeConnection());
    if (adopted_) {
      // The server already sent its one-shot "advertise" burst and will not re-send it. The
      // dialog discovered the FULL channel catalog during that burst (not just what the user
      // selected); seeding from it is the only way to learn about unselected topics here.
      for (const auto& ch : dialog_.allDiscoveredChannels()) {
        advertised_channels_[ch.id] = ch;
      }
    } else if (auto status = connection_.start(dialog_.endpoint(), subprotocols()); !status) {
      return status;
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
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Applies the queued events; re-dials a lost connection every FoxgloveConnectionOptions::reconnect.
    if (auto status = connection_.poll(); !status) {
      return status;  // the first connection of this start failed: stops the stream
    }

    // Demand mode: apply the host's latest desired active-topic set, if it wrote one
    // since the last poll. demand_mode_ and last_applied_desired_ deliberately survive
    // a reconnect (host capability doesn't change mid-session) — the channels that got
    // cleared on reconnect re-subscribe against last_applied_desired_ as they
    // re-advertise (reconcileSubscriptions runs after every advertise delta), so no
    // special reconnect handling is needed here.
    if (demand_mode_ && connection_.isOpen()) {
      applyDesiredTopics();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (connection_.isOpen() && !subscriptions_.empty()) {
      std::vector<uint32_t> ids;
      for (const auto& [id, _channel_id] : subscriptions_) {
        ids.push_back(id);
      }
      connection_.sendText(buildUnsubscribeMessage(ids));
    }
    connection_.stop();  // nothing from this session reaches the next
    selected_channels_.clear();
    subscriptions_.clear();
    binding_by_subscription_.clear();
    advertised_channels_.clear();
    next_subscription_id_ = 1;
    adopted_ = false;
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
    PJ::sdk::arrayLimitToJson(parser_cfg, array_limit_);
    parser_cfg["use_timestamp"] = use_timestamp_;
    parser_cfg["use_embedded_timestamp"] = use_timestamp_;
    parser_cfg["schema_encoding"] = route.parser_encoding;
    if (route.label_keyed_arrays) {
      parser_cfg["label_keyed_arrays"] = true;
    }

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
      if (!passesAdvertiseFilter(ch.topic, selected_channels_) && !isAlwaysSubscribed(ch)) {
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

    auto desired = alwaysSubscribedTopics(advertised_channels_);
    desired.insert(last_applied_desired_.begin(), last_applied_desired_.end());
    const auto diff = computeSubscriptionDiff(subscriptions_, advertised_topics, desired);

    if (!diff.to_unsubscribe.empty()) {
      connection_.sendText(buildUnsubscribeMessage(diff.to_unsubscribe));
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
        connection_.sendText(buildSubscribeMessage(to_send));
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
  // Only for a connection adopted from the dialog: the server will not re-send "advertise", and
  // the selection's channel ids are this server session's. A fresh connection subscribes from
  // the advertise burst instead, by topic, since saved ids may belong to an earlier session.
  void subscribeToSelectedChannels() {
    std::vector<std::string> parser_errors;

    // The always-subscribed channels of the dialog's catalog (seeded into advertised_channels_)
    // join the selection.
    std::vector<ChannelInfo> channels = selected_channels_;
    for (const auto& [_id, ch] : advertised_channels_) {
      if (isAlwaysSubscribed(ch) && std::none_of(channels.begin(), channels.end(), [&](const ChannelInfo& sel) {
            return sel.topic == ch.topic;
          })) {
        channels.push_back(ch);
      }
    }

    for (const auto& ch : channels) {
      if (!classifyChannel(ch).supported) {
        parser_errors.push_back(ch.topic + " (" + ch.schema_encoding + "): unsupported encoding");
        continue;
      }

      advertised_channels_[ch.id] = ch;

      const uint32_t sub_id = registerSubscription(ch, parser_errors);
      connection_.sendText(buildSubscribeMessage({{sub_id, ch.id}}));
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
      const auto channels = parseAdvertisedChannels(json);
      std::vector<std::string> parser_errors;

      for (const auto& ch : channels) {
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
        if (!user_selected && !isAlwaysSubscribed(ch)) {
          continue;
        }
        if (!classifyChannel(ch).supported) {
          parser_errors.push_back(ch.topic + " (" + ch.schema_encoding + "): unsupported encoding");
          continue;
        }

        const uint32_t sub_id = registerSubscription(ch, parser_errors);
        connection_.sendText(buildSubscribeMessage({{sub_id, ch.id}}));
      }

      reportParserErrors(parser_errors);

      // Even an empty advertise is published: after a reconnect it is what clears the previous
      // session's topics from the host.
      if (demand_mode_) {
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
        if (!id_json.is_number_unsigned()) {
          continue;  // malformed: apply the valid ids, so the host's topic list stays in sync
        }
        const uint64_t removed_id = id_json.get<uint64_t>();
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

  // [poll thread] The socket opened, or reopened after a loss. Comes before the server's
  // serverInfo + "advertise" burst in the queue, so state reset here never wipes a catalog that
  // burst populated.
  void onOpen(bool reconnected) {
    if (!reconnected) {
      if (!demand_mode_ && adopted_) {
        subscribeToSelectedChannels();
      }
      return;
    }
    // A fresh server session: reset subscription state so new advertise messages create fresh
    // bindings.
    subscriptions_.clear();
    binding_by_subscription_.clear();
    advertised_channels_.clear();
    next_subscription_id_ = 1;
  }

  // [poll thread] One Foxglove binary frame: push a message-data frame to its subscription's parser.
  void onBinaryMessage(std::string&& message) {
    BinaryFrame frame;
    if (!parseBinaryFrame(reinterpret_cast<const uint8_t*>(message.data()), message.size(), frame)) {
      return;
    }
    auto it = binding_by_subscription_.find(frame.subscription_id);
    if (it == binding_by_subscription_.end()) {
      return;
    }
    // The queued frame itself anchors the payload, so the PayloadView fetcher stays valid after
    // onPoll returns (ObjectIngestPolicy may defer dispatch) without copying it. The span is
    // re-derived from the moved string by offset: a short string's bytes move with it.
    const auto offset = static_cast<size_t>(frame.payload_data - reinterpret_cast<const uint8_t*>(message.data()));
    auto frame_buffer = std::make_shared<const std::string>(std::move(message));
    const PJ::Span<const uint8_t> bytes{
        reinterpret_cast<const uint8_t*>(frame_buffer->data()) + offset, frame.payload_size};
    auto status = runtimeHost().pushMessage(
        it->second, PJ::Timestamp{static_cast<int64_t>(frame.log_time_ns)},
        [frame_buffer, bytes]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{bytes, frame_buffer}; });
    if (!status) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, "Failed to push message: " + status.error());
    }
  }

  FoxgloveDialog dialog_;
  // The connection of this start; its handlers run on the poll thread, from onPoll().
  PJ::WebSocket::SourceConnection connection_;

  PJ::sdk::ArrayLimit array_limit_;
  bool use_timestamp_ = false;

  std::vector<ChannelInfo> selected_channels_;
  bool adopted_ = false;  ///< this start took over the dialog's connection

  std::map<uint32_t, uint64_t> subscriptions_;  // sub_id -> channel_id
  std::map<uint32_t, PJ::ParserBindingHandle> binding_by_subscription_;
  std::map<uint64_t, ChannelInfo> advertised_channels_;  // channel_id -> full channel info (schema included)
  uint32_t next_subscription_id_ = 1;

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
    self->desired_topics_slot_.set(PJ::sdk::stringSetFromViews(names, count));
    return true;
  }
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(FoxgloveSource, kFoxgloveManifest)

PJ_DIALOG_PLUGIN(FoxgloveDialog, kFoxgloveManifest)
