#include <pj_base/sdk/data_source_patterns.hpp>

#include "mqtt_dialog.hpp"

#include <nlohmann/json.hpp>

#include <mqtt/async_client.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct MqttMessage {
  std::string topic;
  std::vector<uint8_t> payload;
  int64_t timestamp_ns;
};

class MqttSource : public PJ::StreamSourceBase {
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
    // Read config from dialog
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    broker_address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 1883);
    topic_filter_ = cfg.value("topics", std::string("#"));
    qos_ = cfg.value("qos", 0);
    client_id_ = cfg.value("client_id", std::string("plotjuggler_mqtt"));
    default_encoding_ = cfg.value("default_encoding", std::string("json"));

    std::string server_uri = "tcp://" + broker_address_ + ":" + std::to_string(port_);

    try {
      client_ = std::make_unique<mqtt::async_client>(server_uri, client_id_);

      // Set up callback for incoming messages
      client_->set_message_callback(
          [this](mqtt::const_message_ptr msg) {
            MqttMessage m;
            m.topic = msg->get_topic();
            auto payload = msg->get_payload();
            m.payload.assign(
                reinterpret_cast<const uint8_t*>(payload.data()),
                reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());
            auto now = std::chrono::system_clock::now().time_since_epoch();
            m.timestamp_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push(std::move(m));
          });

      mqtt::connect_options conn_opts;
      conn_opts.set_clean_session(true);
      conn_opts.set_connect_timeout(std::chrono::seconds(5));

      client_->connect(conn_opts)->wait();
      client_->subscribe(topic_filter_, qos_)->wait();

    } catch (const mqtt::exception& e) {
      return PJ::unexpected(std::string("MQTT error: ") + e.what());
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    std::queue<MqttMessage> batch;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      std::swap(batch, message_queue_);
    }

    while (!batch.empty()) {
      auto& msg = batch.front();

      // Look up or create parser binding for this topic (cached)
      auto it = binding_cache_.find(msg.topic);
      if (it == binding_cache_.end()) {
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = msg.topic,
            .parser_encoding = default_encoding_,
            .type_name = {},
            .schema = {},
            .parser_config_json = {},
        });
        if (binding) {
          it = binding_cache_.emplace(msg.topic, *binding).first;
        }
      }

      if (it != binding_cache_.end()) {
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
    if (client_) {
      try {
        if (client_->is_connected()) {
          client_->unsubscribe(topic_filter_)->wait();
          client_->disconnect()->wait();
        }
      } catch (...) {
      }
      client_.reset();
    }
    binding_cache_.clear();
  }

 private:
  MqttDialog dialog_;

  std::string broker_address_ = "localhost";
  int port_ = 1883;
  std::string topic_filter_ = "#";
  int qos_ = 0;
  std::string client_id_ = "plotjuggler_mqtt";
  std::string default_encoding_ = "json";

  std::unique_ptr<mqtt::async_client> client_;
  std::mutex queue_mutex_;
  std::queue<MqttMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MqttSource,
                       R"({"name":"MQTT Subscriber","version":"1.0.0"})")

PJ_DIALOG_PLUGIN(MqttDialog)
