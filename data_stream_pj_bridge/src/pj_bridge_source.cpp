#include <pj_base/sdk/data_source_patterns.hpp>

#include "pj_bridge_dialog.hpp"
#include "pj_bridge_protocol.hpp"

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

using namespace PJ::BridgeProtocol;

struct QueuedFrame {
  std::vector<uint8_t> data;
};

class PjBridgeSource : public PJ::StreamSourceBase {
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

    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = cfg.value("port", 9871);
    max_array_size_ = cfg.value("max_array_size", 100);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", false);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Read selected topics with schema info from dialog config
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

    socket_ = std::make_unique<QWebSocket>();

    QObject::connect(socket_.get(), &QWebSocket::textMessageReceived,
                     [this](const QString& message) { onTextMessage(message); });

    QObject::connect(socket_.get(), &QWebSocket::binaryMessageReceived,
                     [this](const QByteArray& message) { onBinaryMessage(message); });

    QUrl url(QString("ws://%1:%2").arg(QString::fromStdString(address_)).arg(port_));
    socket_->open(url);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (socket_->state() != QAbstractSocket::ConnectedState &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    if (socket_->state() != QAbstractSocket::ConnectedState) {
      return PJ::unexpected(std::string("failed to connect to PJ bridge at ") +
                            address_ + ":" + std::to_string(port_));
    }

    // Create parser bindings for selected topics
    for (const auto& info : selected_topics_) {
      auto schema_bytes = reinterpret_cast<const uint8_t*>(info.schema.data());

      // Build parser config with array size policy
      nlohmann::json parser_cfg;
      parser_cfg["max_array_size"] = max_array_size_;
      parser_cfg["clamp_large_arrays"] = clamp_large_arrays_;
      parser_cfg["use_timestamp"] = use_timestamp_;

      auto binding = runtimeHost().ensureParserBinding({
          .topic_name = info.name,
          .parser_encoding = info.encoding,
          .type_name = info.schema_name,
          .schema = PJ::Span<const uint8_t>(schema_bytes, info.schema.size()),
          .parser_config_json = parser_cfg.dump(),
      });
      if (binding) {
        bindings_[info.name] = *binding;
      }
    }

    // Subscribe to selected topics
    nlohmann::json subscribe_cmd;
    subscribe_cmd["command"] = "subscribe";
    subscribe_cmd["protocol_version"] = 1;
    nlohmann::json topic_names = nlohmann::json::array();
    for (const auto& info : selected_topics_) {
      topic_names.push_back(info.name);
    }
    subscribe_cmd["topics"] = topic_names;
    socket_->sendTextMessage(QString::fromStdString(subscribe_cmd.dump()));

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);

    // Process queued binary frames
    std::queue<QueuedFrame> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, frame_queue_);
    }

    while (!batch.empty()) {
      auto& frame = batch.front();

      std::vector<RawMessage> messages;
      if (parseBinaryFrame(frame.data.data(), frame.data.size(),
                           messages, decompress_buffer_)) {
        for (const auto& msg : messages) {
          auto it = bindings_.find(msg.topic_name);
          if (it != bindings_.end()) {
            auto status = runtimeHost().pushRawMessage(
                it->second, PJ::Timestamp{msg.timestamp_ns},
                PJ::Span<const uint8_t>(msg.cdr_data, msg.cdr_size));
            if (!status) {
              runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                         "Failed to push message: " + status.error());
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
      socket_->close();
      socket_.reset();
    }
    bindings_.clear();
    selected_topics_.clear();
  }

 private:
  void onTextMessage(const QString& message) {
    // In streaming mode, text messages are protocol responses (heartbeat acks, etc.)
    // Topic discovery is handled by the dialog — source just streams data.
    (void)message;
  }

  void onBinaryMessage(const QByteArray& message) {
    QueuedFrame frame;
    frame.data.assign(reinterpret_cast<const uint8_t*>(message.constData()),
                      reinterpret_cast<const uint8_t*>(message.constData()) + message.size());
    std::lock_guard<std::mutex> lock(queue_mutex_);
    frame_queue_.push(std::move(frame));
  }

  PjBridgeDialog dialog_;

  std::string address_ = "127.0.0.1";
  int port_ = 9871;
  int max_array_size_ = 100;
  bool clamp_large_arrays_ = false;
  bool use_timestamp_ = false;

  std::vector<TopicInfo> selected_topics_;
  std::unique_ptr<QWebSocket> socket_;
  std::map<std::string, PJ::ParserBindingHandle> bindings_;

  std::mutex queue_mutex_;
  std::queue<QueuedFrame> frame_queue_;
  std::vector<uint8_t> decompress_buffer_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(PjBridgeSource,
                       R"({"name":"PlotJuggler Bridge","version":"1.0.0"})")

PJ_DIALOG_PLUGIN(PjBridgeDialog)
