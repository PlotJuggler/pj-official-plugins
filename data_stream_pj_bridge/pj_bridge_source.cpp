#include <pj_base/sdk/data_source_patterns.hpp>

#include "pj_bridge_dialog.hpp"
#include "pj_bridge_manifest.hpp"
#include "pj_bridge_protocol.hpp"

#include <nlohmann/json.hpp>

#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
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
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog | PJ::kCapabilitySupportsPause;
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
        if (state == ix::ReadyState::Open) break;
        if (state == ix::ReadyState::Closed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      if (socket_->getReadyState() != ix::ReadyState::Open) {
        socket_->stop();
        return PJ::unexpected(std::string("failed to connect to PJ bridge at ") +
                              address_ + ":" + std::to_string(port_));
      }
    }

    // Re-register the message callback for the streaming source
    socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Message) {
        if (msg->binary) {
          onBinaryMessage(msg->str);
        } else {
          onTextMessage(msg->str);
        }
      }
    });

    // Subscribe to selected topics — the response contains schemas needed for parsing
    nlohmann::json subscribe_cmd;
    subscribe_cmd["command"] = "subscribe";
    subscribe_cmd["id"] = generateRequestId();
    subscribe_cmd["protocol_version"] = 1;
    nlohmann::json topic_names = nlohmann::json::array();
    for (const auto& info : selected_topics_) {
      topic_names.push_back(info.name);
    }
    subscribe_cmd["topics"] = topic_names;
    subscribe_response_received_ = false;
    socket_->sendText(subscribe_cmd.dump());

    // Wait for the subscribe response (contains schemas)
    auto sub_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!subscribe_response_received_ &&
           std::chrono::steady_clock::now() < sub_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!subscribe_response_received_) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                  "Subscribe response not received; parsers may lack schemas");
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Heartbeat: send every ~1s (30 polls at 33ms each)
    if (socket_ && ++heartbeat_tick_ >= 30) {
      heartbeat_tick_ = 0;
      socket_->sendText(buildRequest("heartbeat", generateRequestId()));
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
      socket_->stop();
      socket_.reset();
    }
    bindings_.clear();
    selected_topics_.clear();
  }

 private:
  void onTextMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return;

    auto status = json.value("status", std::string{});

    // Handle subscribe response: extract schemas and create parser bindings
    if (json.contains("schemas") && (status == "success" || status == "partial_success")) {
      auto schemas = json["schemas"];
      if (schemas.is_object()) {
        nlohmann::json parser_cfg;
        parser_cfg["max_array_size"] = max_array_size_;
        parser_cfg["clamp_large_arrays"] = clamp_large_arrays_;
        parser_cfg["use_timestamp"] = use_timestamp_;
        std::string parser_cfg_str = parser_cfg.dump();

        for (auto it = schemas.begin(); it != schemas.end(); ++it) {
          const std::string& topic_name = it.key();
          auto& schema_obj = it.value();
          std::string encoding = schema_obj.value("encoding", std::string("cdr"));
          std::string definition = schema_obj.value("definition", std::string{});

          // Find schema_name from selected_topics_ (the type name)
          std::string schema_name;
          for (const auto& info : selected_topics_) {
            if (info.name == topic_name) {
              schema_name = info.schema_name;
              break;
            }
          }

          auto schema_bytes = reinterpret_cast<const uint8_t*>(definition.data());
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
            runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                       "Failed to create parser for " + topic_name + ": " + binding.error());
          }
        }
      }

      // Report any failures from partial_success
      if (json.contains("failures") && json["failures"].is_array()) {
        for (const auto& f : json["failures"]) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              "Subscription failed: " + f.value("topic", std::string{"?"}) +
              " — " + f.value("reason", std::string{"unknown"}));
        }
      }

      subscribe_response_received_ = true;
    }
  }

  void onBinaryMessage(const std::string& message) {
    QueuedFrame frame;
    frame.data.assign(reinterpret_cast<const uint8_t*>(message.data()),
                      reinterpret_cast<const uint8_t*>(message.data()) + message.size());
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
  std::unique_ptr<ix::WebSocket> socket_;
  std::map<std::string, PJ::ParserBindingHandle> bindings_;

  std::mutex queue_mutex_;
  std::queue<QueuedFrame> frame_queue_;
  std::vector<uint8_t> decompress_buffer_;
  int heartbeat_tick_ = 0;
  bool paused_ = false;
  std::atomic<bool> subscribe_response_received_ = false;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(PjBridgeSource, kPjBridgeManifest)

PJ_DIALOG_PLUGIN(PjBridgeDialog)
