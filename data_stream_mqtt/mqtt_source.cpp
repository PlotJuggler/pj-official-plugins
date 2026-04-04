#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/encoding_utils.hpp>

#include "mqtt_dialog.hpp"
#include "mqtt_manifest.hpp"

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
    // Always populate available encodings first (needed even if config is empty)
    dialog_.setAvailableEncodings(PJ::sdk::parseEncodingsJson(runtimeHost().listAvailableEncodings()));

    // Load config if provided (empty config on first run is OK)
    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);  // Ignore errors, use defaults
    }

    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    // Read config from dialog
    auto config_str = dialog_.saveConfig();
    auto cfg = nlohmann::json::parse(config_str, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }

    broker_address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 1883);
    topic_filter_ = cfg.value("topics", std::string("#"));
    qos_ = cfg.value("qos", 0);
    client_id_ = cfg.value("client_id", std::string("plotjuggler_mqtt"));
    default_encoding_ = cfg.value("default_encoding", std::string("json"));
    protocol_version_ = cfg.value("protocol_version", 1);  // 0=3.1, 1=3.1.1, 2=5.0

    // Read selected topics (from dialog discovery)
    selected_topics_.clear();
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      for (const auto& t : cfg["selected_topics"]) {
        if (t.is_string()) selected_topics_.push_back(t.get<std::string>());
      }
    }

    bool use_ssl = cfg.value("use_ssl", false);
    std::string scheme = use_ssl ? "ssl://" : "tcp://";
    std::string server_uri = scheme + broker_address_ + ":" + std::to_string(port_);

    std::string username = cfg.value("username", std::string{});
    std::string password = cfg.value("password", std::string{});

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
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            m.timestamp_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push(std::move(m));
          });

      // Detect connection loss
      client_->set_connection_lost_handler([this](const std::string& cause) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                   "MQTT connection lost" + (cause.empty() ? "" : ": " + cause));
      });

      mqtt::connect_options conn_opts;
      conn_opts.set_clean_session(true);
      conn_opts.set_connect_timeout(std::chrono::seconds(5));

      // MQTT protocol version
      if (protocol_version_ == 0) {
        conn_opts.set_mqtt_version(MQTTVERSION_3_1);
      } else if (protocol_version_ == 2) {
        conn_opts.set_mqtt_version(MQTTVERSION_5);
      } else {
        conn_opts.set_mqtt_version(MQTTVERSION_3_1_1);
      }

      // Authentication
      if (!username.empty()) {
        conn_opts.set_user_name(username);
        conn_opts.set_password(password);
      }

      // TLS certificates
      if (use_ssl) {
        mqtt::ssl_options ssl_opts;
        auto ca = cfg.value("ca_cert_path", std::string{});
        auto cert = cfg.value("client_cert_path", std::string{});
        auto key = cfg.value("private_key_path", std::string{});
        if (!ca.empty()) ssl_opts.set_trust_store(ca);
        if (!cert.empty()) ssl_opts.set_key_store(cert);
        if (!key.empty()) ssl_opts.set_private_key(key);
        conn_opts.set_ssl(ssl_opts);
      }

      client_->connect(conn_opts)->wait();

      // Subscribe to selected topics from dialog, or fall back to topic filter
      if (!selected_topics_.empty()) {
        for (const auto& topic : selected_topics_) {
          client_->subscribe(topic, qos_)->wait();
        }
      } else {
        client_->subscribe(topic_filter_, qos_)->wait();
      }

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
          if (!selected_topics_.empty()) {
            for (const auto& topic : selected_topics_) {
              client_->unsubscribe(topic)->wait();
            }
          } else {
            client_->unsubscribe(topic_filter_)->wait();
          }
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
  int protocol_version_ = 1;
  std::vector<std::string> selected_topics_;

  std::unique_ptr<mqtt::async_client> client_;
  std::mutex queue_mutex_;
  std::queue<MqttMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MqttSource, kMqttManifest)

PJ_DIALOG_PLUGIN(MqttDialog)
