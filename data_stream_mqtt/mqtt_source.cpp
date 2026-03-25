#include <pj_base/sdk/data_source_patterns.hpp>

#include "mqtt_dialog.hpp"
#include "mqtt_manifest.hpp"

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <nlohmann/json.hpp>

#include <mqtt/async_client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/// Compiles a .proto file into serialized FileDescriptorSet bytes.
/// Returns empty vector on failure, setting error_out.
static std::vector<uint8_t> compileProtoFile(const std::string& proto_path,
                                              const std::vector<std::string>& include_folders,
                                              std::string& error_out) {
  namespace fs = std::filesystem;
  fs::path p(proto_path);
  std::string dir = p.parent_path().string();
  std::string filename = p.filename().string();

  google::protobuf::compiler::DiskSourceTree source_tree;
  source_tree.MapPath("", dir);
  for (const auto& folder : include_folders) {
    source_tree.MapPath("", folder);
  }

  google::protobuf::compiler::Importer importer(&source_tree, nullptr);
  const google::protobuf::FileDescriptor* fd = importer.Import(filename);
  if (!fd) {
    error_out = "Failed to compile proto file: " + proto_path;
    return {};
  }

  google::protobuf::FileDescriptorSet fds;
  // Collect all transitive dependencies
  std::function<void(const google::protobuf::FileDescriptor*)> collect =
      [&](const google::protobuf::FileDescriptor* f) {
        for (int i = 0; i < f->dependency_count(); ++i) {
          collect(f->dependency(i));
        }
        f->CopyTo(fds.add_file());
      };
  collect(fd);

  std::string serialized = fds.SerializeAsString();
  return std::vector<uint8_t>(serialized.begin(), serialized.end());
}

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
    auto raw_config = dialog_.saveConfig();
    auto cfg = nlohmann::json::parse(raw_config, nullptr, false);
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
    proto_file_path_ = cfg.value("proto_file_path", std::string{});
    proto_message_type_ = cfg.value("proto_message_type", std::string{});
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    include_folders_.clear();
    if (cfg.contains("include_folders") && cfg["include_folders"].is_array()) {
      for (const auto& f : cfg["include_folders"]) {
        if (f.is_string()) include_folders_.push_back(f.get<std::string>());
      }
    }

    // Compile .proto schema if encoding is protobuf
    proto_schema_.clear();
    if (default_encoding_ == "protobuf") {
      if (proto_file_path_.empty()) {
        return PJ::unexpected("Protobuf encoding requires a .proto file. Set it in the dialog.");
      }
      if (proto_message_type_.empty()) {
        return PJ::unexpected("Protobuf encoding requires a message type name. Set it in the dialog.");
      }
      std::string compile_error;
      proto_schema_ = compileProtoFile(proto_file_path_, include_folders_, compile_error);
      if (proto_schema_.empty()) {
        return PJ::unexpected(compile_error);
      }
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo,
                                 "Protobuf schema loaded: " + proto_message_type_ +
                                     " (" + std::to_string(proto_schema_.size()) + " bytes)");
    }

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
            auto now = std::chrono::system_clock::now().time_since_epoch();
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
        std::string parser_cfg;
        if (default_encoding_ == "protobuf" && use_embedded_timestamp_) {
          parser_cfg = "{\"use_embedded_timestamp\":true}";
        }
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = msg.topic,
            .parser_encoding = default_encoding_,
            .type_name = proto_message_type_,
            .schema = PJ::Span<const uint8_t>(proto_schema_.data(), proto_schema_.size()),
            .parser_config_json = parser_cfg,
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
  std::string proto_file_path_;
  std::string proto_message_type_;
  bool use_embedded_timestamp_ = false;
  std::vector<std::string> include_folders_;
  std::vector<uint8_t> proto_schema_;

  std::unique_ptr<mqtt::async_client> client_;
  std::mutex queue_mutex_;
  std::queue<MqttMessage> message_queue_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(MqttSource, kMqttManifest)

PJ_DIALOG_PLUGIN(MqttDialog)
