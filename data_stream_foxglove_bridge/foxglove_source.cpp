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
#include <pj_base64/base64.hpp>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "foxglove_dialog.hpp"
#include "foxglove_manifest.hpp"
#include "foxglove_protocol.hpp"

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
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog | PJ::kCapabilityLazySubscription;
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
        ChannelInfo info = channelFromConfigJson(ch);
        if (!info.topic.empty()) {
          selected_channels_.push_back(std::move(info));
        }
      }
    }

    // Full channel catalog captured by the dialog from the initial advertise
    // burst (the stolen socket never re-receives it). Basis for the lazy-mode
    // advertise-all; falls back to the selected channels when a config saved
    // by an older dialog lacks it.
    channel_catalog_.clear();
    if (cfg.contains("all_channels") && cfg["all_channels"].is_array()) {
      for (const auto& ch : cfg["all_channels"]) {
        ChannelInfo info = channelFromConfigJson(ch);
        if (!info.topic.empty()) {
          channel_catalog_[info.id] = std::move(info);
        }
      }
    }
    if (channel_catalog_.empty()) {
      for (const auto& ch : selected_channels_) {
        channel_catalog_[ch.id] = ch;
      }
    }

    // Steal the live socket from the dialog (it stays connected on accept).
    socket_ = dialog_.takeSocket();

    if (!socket_ || socket_->getReadyState() != ix::ReadyState::Open) {
      // Fallback: connect fresh (e.g. when started without dialog via saved config)
      socket_ = std::make_unique<ix::WebSocket>();
      socket_->setUrl("ws://" + address_ + ":" + std::to_string(port_));
      socket_->addSubProtocol("foxglove.sdk.v1");
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
            std::string("failed to connect to Foxglove bridge at ") + address_ + ":" + std::to_string(port_));
      }
    }

    // Re-register message callback for the streaming source.
    // Text messages (advertise/unadvertise) are queued and processed in onPoll()
    // so that ensureParserBinding() is always called from the poll thread.
    socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Message) {
        if (msg->binary) {
          onBinaryMessage(msg->str);
        } else {
          std::lock_guard<std::mutex> lock(text_queue_mutex_);
          text_queue_.push({msg->str});
        }
      } else if (msg->type == ix::WebSocketMessageType::Close) {
        onDisconnected();
      }
    });

    // When the socket is stolen from the dialog it is already connected and the server
    // already sent its initial "advertise" burst — it will not re-send it.
    //
    // Lazy mode: advertise the FULL catalog to the host without subscribing;
    // the host drives per-channel subscriptions from consumer demand, with
    // the dialog-selected channels as an always-on eager floor. On a pre-0.15
    // host the advertise fails with a distinct error and we fall back to the
    // eager subscribe-selected behavior unchanged.
    lazy_mode_ = advertiseCatalogToHost();
    if (!lazy_mode_ && selected_channels_.empty()) {
      // Old host without lazy-mode support and no dialog-selected channels —
      // there is nothing to subscribe to and nothing will ever appear later,
      // so fail loudly here instead of streaming nothing forever. Mirrors
      // the ROS 2 source's `!lazy_mode_ && selected_topics_.empty()` guard.
      socket_->stop();
      socket_.reset();
      return PJ::unexpected("no Foxglove channels selected");
    }
    if (lazy_mode_) {
      reconcileSubscriptions();
    } else {
      subscribeToSelectedChannels();
    }

    connected_ = true;
    return PJ::okStatus();
  }

  PJ::Status onActiveTopicsChanged(PJ::Span<const std::string_view> active_topics) override {
    host_active_.clear();
    for (const std::string_view name : active_topics) {
      host_active_.insert(std::string(name));
    }
    if (lazy_mode_ && connected_) {
      reconcileSubscriptions();
    }
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Drain text messages (advertise/unadvertise) queued from the WebSocket callback thread.
    // ensureParserBinding() must be called from the poll thread, not the callback thread.
    std::queue<QueuedTextMessage> text_batch;
    {
      std::lock_guard<std::mutex> lock(text_queue_mutex_);
      std::swap(text_batch, text_queue_);
    }
    while (!text_batch.empty()) {
      onTextMessage(text_batch.front().text);
      text_batch.pop();
    }

    // Non-blocking reconnection: if connection was lost, try every ~5s
    if (!connected_ && socket_) {
      if (reconnect_pending_) {
        // Check if the async reconnect succeeded
        if (socket_->getReadyState() == ix::ReadyState::Open) {
          connected_ = true;
          reconnect_pending_ = false;
          reconnect_tick_ = 0;
          // Reset subscription state so new advertise messages create fresh bindings.
          // In lazy mode the catalog also resets — the fresh advertise burst
          // rebuilds it and re-advertises to the host; host_active_ persists,
          // so wanted topics re-subscribe automatically on the reconcile.
          subscriptions_.clear();
          binding_by_subscription_.clear();
          advertised_channels_.clear();
          if (lazy_mode_) {
            channel_catalog_.clear();
          }
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

    std::queue<QueuedBinaryMessage> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, message_queue_);
    }

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
    channel_catalog_.clear();
    host_active_.clear();
    lazy_mode_ = false;
    next_subscription_id_ = 1;
  }

 private:
  static ChannelInfo channelFromConfigJson(const nlohmann::json& ch) {
    ChannelInfo info;
    info.id = ch.value("id", uint64_t{0});
    info.topic = ch.value("topic", "");
    info.encoding = ch.value("encoding", "");
    info.schema_name = ch.value("schema_name", "");
    info.schema = ch.value("schema", "");
    info.schema_encoding = ch.value("schema_encoding", "");
    return info;
  }

  // Parser config forwarded with every binding/advertise for one route —
  // identical for bindChannel and advertiseCatalogToHost so the host's
  // per-topic binding dedupe sees one consistent request per topic.
  std::string parserConfigFor(const ChannelRoute& route) const {
    nlohmann::json parser_cfg;
    pj::array_policy::arrayLimitToJson(parser_cfg, array_limit_);
    parser_cfg["use_timestamp"] = use_timestamp_;
    parser_cfg["use_embedded_timestamp"] = use_timestamp_;
    parser_cfg["schema_encoding"] = route.parser_encoding;
    return parser_cfg.dump();
  }

  // Foxglove base64-encodes binary schemas (the protobuf FileDescriptorSet) in
  // the advertise JSON, while text schemas (ros2msg/omgidl) arrive verbatim.
  // `decoded_storage` must outlive the host call consuming the span.
  static PJ::Span<const uint8_t> schemaSpanFor(
      const ChannelInfo& ch, const ChannelRoute& route, std::string& decoded_storage) {
    if (route.schema_is_base64) {
      decoded_storage = PJ::base64::decode(ch.schema);
      return PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(decoded_storage.data()), decoded_storage.size());
    }
    return PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(ch.schema.data()), ch.schema.size());
  }

  // Create a parser binding for a supported channel and record it for its
  // subscription id. Returns an error string on binding failure, std::nullopt
  // on success. Shared by both subscribe paths (stolen-socket and fresh-connect).
  std::optional<std::string> bindChannel(const ChannelInfo& ch, uint32_t sub_id) {
    const ChannelRoute route = classifyChannel(ch);

    std::string decoded;
    const PJ::Span<const uint8_t> schema_span = schemaSpanFor(ch, route, decoded);

    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = ch.topic,
        .parser_encoding = route.parser_encoding,
        .type_name = ch.schema_name,
        .schema = schema_span,
        .parser_config_json = parserConfigFor(route),
    });
    if (!binding) {
      return ch.topic + " (" + route.parser_encoding + "): " + binding.error();
    }
    binding_by_subscription_[sub_id] = *binding;
    return std::nullopt;
  }

  // Advertise every supported catalog channel to the host WITHOUT
  // subscribing. Returns false when the host predates lazy subscription
  // (distinct setAdvertisedTopics error) — callers then fall back to eager.
  bool advertiseCatalogToHost() {
    std::vector<PJ::ParserBindingRequest> requests;
    std::vector<std::string> decoded_storage;
    std::vector<std::string> config_storage;
    requests.reserve(channel_catalog_.size());
    decoded_storage.reserve(channel_catalog_.size());
    config_storage.reserve(channel_catalog_.size());

    for (const auto& [channel_id, ch] : channel_catalog_) {
      const ChannelRoute route = classifyChannel(ch);
      if (!route.supported) {
        continue;
      }
      decoded_storage.emplace_back();
      config_storage.push_back(parserConfigFor(route));
      requests.push_back(
          PJ::ParserBindingRequest{
              .topic_name = ch.topic,
              .parser_encoding = route.parser_encoding,
              .type_name = ch.schema_name,
              .schema = schemaSpanFor(ch, route, decoded_storage.back()),
              .parser_config_json = config_storage.back(),
          });
    }

    auto status =
        runtimeHost().setAdvertisedTopics(PJ::Span<const PJ::ParserBindingRequest>(requests.data(), requests.size()));
    if (!status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kInfo, "Lazy subscription unavailable (" + status.error() +
                                                 ") — falling back to eager subscription of selected channels");
      return false;
    }
    return true;
  }

  // Declarative reconcile: desired = dialog-selected eager floor ∪ the
  // host's active set, mapped to supported catalog channels. Newly desired
  // channels get bound + subscribed; no-longer-desired ones unsubscribed
  // (their host-side bindings and data persist).
  void reconcileSubscriptions() {
    std::set<uint64_t> desired_channels;
    for (const auto& [channel_id, ch] : channel_catalog_) {
      if (!classifyChannel(ch).supported) {
        continue;
      }
      bool desired = host_active_.count(ch.topic) > 0;
      if (!desired) {
        for (const auto& sel : selected_channels_) {
          if (sel.topic == ch.topic) {
            desired = true;
            break;
          }
        }
      }
      if (desired) {
        desired_channels.insert(channel_id);
      }
    }

    const SubscriptionOps ops = computeSubscriptionOps(subscriptions_, desired_channels, next_subscription_id_);
    if (!ops.to_unsubscribe.empty()) {
      socket_->sendText(buildUnsubscribeMessage(ops.to_unsubscribe));
      for (const uint32_t sub_id : ops.to_unsubscribe) {
        subscriptions_.erase(sub_id);
        binding_by_subscription_.erase(sub_id);
      }
    }
    if (!ops.to_subscribe.empty()) {
      std::vector<std::string> parser_errors;
      for (const auto& [sub_id, channel_id] : ops.to_subscribe) {
        const auto ch = channel_catalog_.find(channel_id);
        if (ch == channel_catalog_.end()) {
          continue;
        }
        subscriptions_[sub_id] = channel_id;
        if (auto err = bindChannel(ch->second, sub_id)) {
          parser_errors.push_back(*err);
        }
        socket_->sendText(buildSubscribeMessage({{sub_id, channel_id}}));
      }
      reportParserErrors(parser_errors);
    }
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

      advertised_channels_[ch.id] = ch.topic;

      uint32_t sub_id = next_subscription_id_++;
      subscriptions_[sub_id] = ch.id;

      if (auto err = bindChannel(ch, sub_id)) {
        parser_errors.push_back(*err);
      }

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

        // Track server-advertised channels for unadvertise
        advertised_channels_[ch.id] = ch.topic;
        channel_catalog_[ch.id] = ch;

        // Lazy mode: subscriptions are driven by the reconcile below, not here.
        if (lazy_mode_) {
          continue;
        }

        // Only subscribe to channels that were selected in the dialog
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

        uint32_t sub_id = next_subscription_id_++;
        subscriptions_[sub_id] = ch.id;

        if (auto err = bindChannel(ch, sub_id)) {
          parser_errors.push_back(*err);
        }

        socket_->sendText(buildSubscribeMessage({{sub_id, ch.id}}));
      }

      reportParserErrors(parser_errors);

      // Mid-stream advertise in lazy mode: refresh the host's catalog and
      // reconcile — a newly advertised topic the host already wants (e.g. a
      // pending layout curve) subscribes immediately.
      if (lazy_mode_ && !channels_arr.empty()) {
        (void)advertiseCatalogToHost();
        reconcileSubscriptions();
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
          removed_topics.push_back(it->second);
          advertised_channels_.erase(it);
        }
        channel_catalog_.erase(removed_id);
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
        // Lazy mode: report the shrunken catalog so the host marks the
        // removed topics unavailable (their data persists host-side).
        if (lazy_mode_) {
          (void)advertiseCatalogToHost();
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

    std::lock_guard<std::mutex> lock(queue_mutex_);
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
  std::map<uint64_t, std::string> advertised_channels_;  // channel_id -> topic name
  uint32_t next_subscription_id_ = 1;

  // Lazy subscription: full channel catalog (advertised to the host), the
  // host's desired-active topic set, and whether the host accepted lazy mode.
  // All touched only on the poll/start thread (the extension contract).
  std::map<uint64_t, ChannelInfo> channel_catalog_;
  std::set<std::string> host_active_;
  bool lazy_mode_ = false;

  std::mutex queue_mutex_;
  std::queue<QueuedBinaryMessage> message_queue_;
  std::mutex text_queue_mutex_;
  std::queue<QueuedTextMessage> text_queue_;
  int reconnect_tick_ = 0;
  std::atomic<bool> reconnect_pending_ = false;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(FoxgloveSource, kFoxgloveManifest)

PJ_DIALOG_PLUGIN(FoxgloveDialog)
