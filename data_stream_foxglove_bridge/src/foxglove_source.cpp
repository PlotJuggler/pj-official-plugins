#include <pj_base/sdk/data_source_patterns.hpp>

#include "foxglove_dialog.hpp"
#include "foxglove_protocol.hpp"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QWebSocket>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace {

using namespace PJ::FoxgloveProtocol;

struct QueuedBinaryMessage {
  uint32_t subscription_id;
  int64_t timestamp_ns;
  std::vector<uint8_t> payload;
};

class FoxgloveSource : public PJ::StreamSourceBase {
 public:
  void* dialogContext() override { return &dialog_; }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

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
    max_array_size_ = cfg.value("max_array_size", 100);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", false);
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

    socket_ = std::make_unique<QWebSocket>();

    QObject::connect(socket_.get(), &QWebSocket::textMessageReceived,
                     [this](const QString& message) { onTextMessage(message); });

    QObject::connect(socket_.get(), &QWebSocket::binaryMessageReceived,
                     [this](const QByteArray& message) { onBinaryMessage(message); });

    QObject::connect(socket_.get(), &QWebSocket::disconnected,
                     [this]() { onDisconnected(); });

    QNetworkRequest request(
        QUrl(QString("ws://%1:%2").arg(QString::fromStdString(address_)).arg(port_)));
    request.setRawHeader("Sec-WebSocket-Protocol", "foxglove.sdk.v1");
    socket_->open(request);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (socket_->state() != QAbstractSocket::ConnectedState &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    if (socket_->state() != QAbstractSocket::ConnectedState) {
      return PJ::unexpected(std::string("failed to connect to Foxglove bridge at ") +
                            address_ + ":" + std::to_string(port_));
    }

    connected_ = true;
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);

    // Reconnection: if connection was lost, try to reconnect every ~5s
    if (!connected_ && socket_ && ++reconnect_tick_ >= 150) {
      reconnect_tick_ = 0;
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Attempting to reconnect to Foxglove bridge...");
      QNetworkRequest request(
          QUrl(QString("ws://%1:%2").arg(QString::fromStdString(address_)).arg(port_)));
      request.setRawHeader("Sec-WebSocket-Protocol", "foxglove.sdk.v1");
      socket_->open(request);

      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (socket_->state() != QAbstractSocket::ConnectedState &&
             std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
      }
      if (socket_->state() == QAbstractSocket::ConnectedState) {
        connected_ = true;
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Reconnected to Foxglove bridge");
        // Server will re-advertise channels, triggering re-subscription in onTextMessage
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
        auto status = runtimeHost().pushRawMessage(
            it->second, PJ::Timestamp{msg.timestamp_ns},
            PJ::Span<const uint8_t>(msg.payload.data(), msg.payload.size()));
        if (!status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                     "Failed to push message: " + status.error());
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
        socket_->sendTextMessage(
            QString::fromStdString(buildUnsubscribeMessage(ids)));
      }

      socket_->close();
      socket_.reset();
    }
    connected_ = false;
    selected_channels_.clear();
    subscriptions_.clear();
    binding_by_subscription_.clear();
    next_subscription_id_ = 1;
  }

 private:
  void onTextMessage(const QString& message) {
    auto json = nlohmann::json::parse(message.toStdString(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) return;

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

        // Only subscribe to channels that were selected in the dialog
        bool user_selected = false;
        for (const auto& sel : selected_channels_) {
          if (sel.topic == ch.topic) {
            user_selected = true;
            break;
          }
        }
        if (!user_selected || !isUsableChannel(ch)) continue;

        uint32_t sub_id = next_subscription_id_++;
        subscriptions_[sub_id] = ch.id;

        // Build parser config with array size policy
        nlohmann::json parser_cfg;
        parser_cfg["max_array_size"] = max_array_size_;
        parser_cfg["clamp_large_arrays"] = clamp_large_arrays_;
        parser_cfg["use_timestamp"] = use_timestamp_;

        auto schema_bytes = reinterpret_cast<const uint8_t*>(ch.schema.data());
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = ch.topic,
            .parser_encoding = ch.encoding,
            .type_name = ch.schema_name,
            .schema = PJ::Span<const uint8_t>(schema_bytes, ch.schema.size()),
            .parser_config_json = parser_cfg.dump(),
        });
        if (binding) {
          binding_by_subscription_[sub_id] = *binding;
        } else {
          parser_errors.push_back(ch.topic + " (" + ch.encoding + "): " + binding.error());
        }

        socket_->sendTextMessage(QString::fromStdString(
            buildSubscribeMessage({{sub_id, ch.id}})));
      }

      // Report all parser binding failures in a single aggregated message
      if (!parser_errors.empty()) {
        std::string msg = "Failed to create parser for " +
                          std::to_string(parser_errors.size()) + " channel(s):\n";
        for (const auto& e : parser_errors) msg += "  - " + e + "\n";
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, msg);
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
        for (const auto& t : removed_topics) msg += " " + t;
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, msg);
      }
    }

    // Handle server status/warning messages
    if (op == "status") {
      int level = json.value("level", 0);
      std::string status_msg = json.value("message", "");
      auto pj_level = (level >= 2) ? PJ::DataSourceMessageLevel::kError
                                   : PJ::DataSourceMessageLevel::kWarning;
      runtimeHost().reportMessage(pj_level, "Foxglove server: " + status_msg);
    }
  }

  void onDisconnected() {
    if (connected_) {
      connected_ = false;
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                  "Foxglove bridge connection lost");
    }
  }

  void onBinaryMessage(const QByteArray& message) {
    BinaryFrame frame;
    if (!parseBinaryFrame(reinterpret_cast<const uint8_t*>(message.constData()),
                          static_cast<size_t>(message.size()), frame)) {
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
  int max_array_size_ = 100;
  bool clamp_large_arrays_ = false;
  bool use_timestamp_ = false;

  std::vector<ChannelInfo> selected_channels_;
  std::unique_ptr<QWebSocket> socket_;
  bool connected_ = false;

  std::map<uint32_t, uint64_t> subscriptions_;  // sub_id -> channel_id
  std::map<uint32_t, PJ::ParserBindingHandle> binding_by_subscription_;
  std::map<uint64_t, std::string> advertised_channels_;  // channel_id -> topic name
  uint32_t next_subscription_id_ = 1;

  std::mutex queue_mutex_;
  std::queue<QueuedBinaryMessage> message_queue_;
  int reconnect_tick_ = 0;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(FoxgloveSource,
                       R"({"name":"Foxglove Bridge","version":"1.0.0"})")

PJ_DIALOG_PLUGIN(FoxgloveDialog)
