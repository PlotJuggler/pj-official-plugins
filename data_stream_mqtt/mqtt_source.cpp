#include <mqtt/async_client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/encoding_utils.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <string>
#include <vector>

#include "mqtt_connection.hpp"
#include "mqtt_dialog.hpp"
#include "mqtt_manifest.hpp"

namespace {

struct MqttMessage {
  std::string topic;
  std::vector<uint8_t> payload;
  int64_t timestamp_ns;
};

class MqttSource : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    // Always populate available encodings first (needed even if config is empty)
    dialog_.setAvailableEncodings(PJ::sdk::parseEncodingsJson(runtimeHost().listAvailableEncodings()));

    // Load config if provided (empty config on first run is OK)
    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);  // Ignore errors, use defaults
    }

    // Capture the parser configuration the app injects under "_parser_config"
    // (e.g. the protobuf descriptor + message type). Streaming sources must
    // forward it to ensureParserBinding, otherwise schema-based parsers bind
    // with no schema and drop every message.
    parser_config_override_ = PJ::sdk::parserConfigOverride(config_json);

    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    // Read config from dialog
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    const auto connection = pj::mqtt_support::connectionSettingsFromJson(cfg);
    topic_filter_ = cfg.value("topics", std::string("#"));
    qos_ = cfg.value("qos", 0);
    client_id_ = cfg.value("client_id", std::string{});
    if (client_id_.empty()) {
      client_id_ = pj::mqtt_support::randomClientId("plotjuggler_mqtt_");
    }
    default_encoding_ = cfg.value("default_encoding", std::string("json"));

    // Read selected topics (from dialog discovery)
    selected_topics_.clear();
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      for (const auto& t : cfg["selected_topics"]) {
        if (t.is_string()) {
          selected_topics_.push_back(t.get<std::string>());
        }
      }
    }

    initial_connect_done_.store(false, std::memory_order_relaxed);
    (void)lost_cause_.take();
    reconnected_.store(false, std::memory_order_relaxed);

    try {
      client_ = std::make_unique<mqtt::async_client>(pj::mqtt_support::brokerUri(connection), client_id_);

      // Set up callback for incoming messages
      client_->set_message_callback([this](mqtt::const_message_ptr msg) {
        MqttMessage m;
        m.topic = msg->get_topic();
        auto payload = msg->get_payload();
        m.payload.assign(
            reinterpret_cast<const uint8_t*>(payload.data()),
            reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());
        auto now = std::chrono::system_clock::now().time_since_epoch();
        m.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        message_queue_.push(std::move(m));
      });

      // These run on paho's internal thread: just latch state for onPoll to act on,
      // no host/API calls here.
      client_->set_connection_lost_handler([this](const std::string& cause) { lost_cause_.set(cause); });
      client_->set_connected_handler([this](const std::string& /*cause*/) {
        // Also fires for the initial connect below, which reports its own errors
        // synchronously; only automatic-reconnect completions should hit onPoll.
        if (initial_connect_done_.load(std::memory_order_relaxed)) {
          reconnected_.store(true, std::memory_order_relaxed);
        }
      });

      auto opts = pj::mqtt_support::makeConnectOptions(connection);
      opts.set_automatic_reconnect(std::chrono::seconds(1), std::chrono::seconds(5));
      client_->connect(opts)->wait();

      for (const auto& topic : topicsToSubscribe()) {
        client_->subscribe(topic, qos_)->wait();
      }

      initial_connect_done_.store(true, std::memory_order_relaxed);

    } catch (const mqtt::exception& e) {
      return PJ::unexpected(std::string("MQTT error: ") + e.what());
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    if (auto cause = lost_cause_.take()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "MQTT connection lost" + (cause->empty() ? "" : ": " + *cause) + "; reconnecting");
    }

    if (reconnected_.exchange(false, std::memory_order_relaxed)) {
      for (const auto& topic : topicsToSubscribe()) {
        try {
          client_->subscribe(topic, qos_);
        } catch (const mqtt::exception& e) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning, "Failed to re-subscribe to " + topic + ": " + e.what());
        }
      }
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "Reconnected to MQTT broker");
    }

    auto batch = message_queue_.drain();

    while (!batch.empty()) {
      auto& msg = batch.front();

      auto status = ingest_.push(
          runtimeHost(), msg.topic,
          {
              .topic_name = msg.topic,
              .parser_encoding = default_encoding_,
              .type_name = {},
              .schema = {},
              .parser_config_json = parser_config_override_,
          },
          PJ::Timestamp{msg.timestamp_ns}, std::move(msg.payload));
      if (!status) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, "Failed to push message: " + status.error());
      }

      batch.pop();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (client_) {
      try {
        // clean_session=true drops subscriptions on disconnect, so no need to
        // unsubscribe first. disconnect() unconditionally (not gated on
        // is_connected()) because it's also what stops paho's automatic-reconnect
        // loop; wait_for() bounds the call so an unresponsive broker can't hang Stop.
        client_->disconnect()->wait_for(std::chrono::seconds(2));
      } catch (...) {}
      client_.reset();
    }
    ingest_.clear();
  }

 private:
  /// Topics to (re)subscribe to: the dialog's discovered selection, or the
  /// broker-side filter as a fallback. Shared by onStart and the onPoll resubscribe.
  std::vector<std::string> topicsToSubscribe() const {
    return selected_topics_.empty() ? std::vector<std::string>{topic_filter_} : selected_topics_;
  }

  MqttDialog dialog_;

  std::string topic_filter_ = "#";
  int qos_ = 0;
  std::string client_id_ = "plotjuggler_mqtt";
  std::string default_encoding_ = "json";
  std::vector<std::string> selected_topics_;
  std::string parser_config_override_;

  std::unique_ptr<mqtt::async_client> client_;
  PJ::sdk::DrainQueue<MqttMessage> message_queue_;
  PJ::sdk::DelegatedIngestCache ingest_;

  // Set by paho's callback thread, consumed by onPoll on the poll thread.
  std::atomic<bool> initial_connect_done_{false};
  std::atomic<bool> reconnected_{false};
  PJ::sdk::LatestValueSlot<std::string> lost_cause_;  // set = connection lost, value = paho's cause
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MqttSource, kMqttManifest)

PJ_DIALOG_PLUGIN(MqttDialog, kMqttManifest)
