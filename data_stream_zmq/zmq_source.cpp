#include <pj_base/sdk/data_source_patterns.hpp>

#include "zmq_dialog.hpp"
#include "zmq_manifest.hpp"

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class ZmqSource : public PJ::StreamSourceBase {
 public:
  void* dialogContext() override { return &dialog_; }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  PJ::Status bindRuntimeHost(PJ_data_source_runtime_host_t runtime_host) override {
    auto status = PJ::StreamSourceBase::bindRuntimeHost(runtime_host);
    if (!status) {
      return status;
    }
    // Wire up the dialog's encoding callback to query the runtime host
    dialog_.setEncodingsCallback([this]() -> std::string_view {
      return runtimeHost().listAvailableEncodings();
    });
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
    // Read config from dialog
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 9872);
    transport_ = cfg.value("transport", std::string("tcp://"));
    connect_mode_ = cfg.value("mode", std::string("connect")) == "connect";
    topic_filter_ = cfg.value("topics", std::string{});
    default_encoding_ = cfg.value("default_encoding", std::string("json"));

    context_ = std::make_unique<zmq::context_t>();
    socket_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::sub);

    endpoint_ = transport_ + address_ + ":" + std::to_string(port_);

    try {
      if (connect_mode_) {
        socket_->connect(endpoint_);
      } else {
        socket_->bind(endpoint_);
      }

      // Subscribe to topics
      if (topic_filter_.empty()) {
        socket_->set(zmq::sockopt::subscribe, "");
      } else {
        // Split by comma/semicolon/whitespace
        std::string current;
        for (char c : topic_filter_) {
          if (c == ',' || c == ';' || c == ' ' || c == '\t') {
            if (!current.empty()) {
              socket_->set(zmq::sockopt::subscribe, current);
              current.clear();
            }
          } else {
            current += c;
          }
        }
        if (!current.empty()) {
          socket_->set(zmq::sockopt::subscribe, current);
        }
      }

      socket_->set(zmq::sockopt::rcvtimeo, 0);  // non-blocking

    } catch (const zmq::error_t& e) {
      return PJ::unexpected(std::string("ZMQ error: ") + e.what());
    }

    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    constexpr int kMaxMessagesPerPoll = 100;

    for (int i = 0; i < kMaxMessagesPerPoll; i++) {
      zmq::message_t recv_msg;
      auto result = socket_->recv(recv_msg, zmq::recv_flags::dontwait);
      if (!result || recv_msg.size() == 0) break;

      // Multi-part: first frame is topic
      std::string topic;
      if (recv_msg.more()) {
        topic = std::string(static_cast<const char*>(recv_msg.data()), recv_msg.size());
        recv_msg.rebuild();
        result = socket_->recv(recv_msg, zmq::recv_flags::dontwait);
        if (!result || recv_msg.size() == 0) continue;
      }

      auto* payload_data = static_cast<const uint8_t*>(recv_msg.data());
      size_t payload_size = recv_msg.size();

      // Optional third frame: timestamp as text (seconds since epoch)
      int64_t timestamp_ns = 0;
      if (recv_msg.more()) {
        zmq::message_t ts_msg;
        result = socket_->recv(ts_msg, zmq::recv_flags::dontwait);
        if (result && ts_msg.size() > 0) {
          std::string ts_str(static_cast<const char*>(ts_msg.data()), ts_msg.size());
          double ts_sec = std::strtod(ts_str.c_str(), nullptr);
          timestamp_ns = static_cast<int64_t>(ts_sec * 1e9);
        }
        // Drain remaining frames
        while (ts_msg.more()) {
          ts_msg.rebuild();
          (void)socket_->recv(ts_msg, zmq::recv_flags::dontwait);
        }
      } else {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
        timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
      }

      // Delegated ingest: push raw payload to parser
      std::string topic_name = topic.empty() ? "zmq/data" : topic;

      auto it = binding_cache_.find(topic_name);
      if (it == binding_cache_.end()) {
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = topic_name,
            .parser_encoding = default_encoding_,
            .type_name = {},
            .schema = {},
            .parser_config_json = {},
        });
        if (binding) {
          it = binding_cache_.emplace(topic_name, *binding).first;
        }
      }

      if (it != binding_cache_.end()) {
        auto status = runtimeHost().pushRawMessage(
            it->second, PJ::Timestamp{timestamp_ns}, PJ::Span<const uint8_t>(payload_data, payload_size));
        if (!status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                     "Failed to push message: " + status.error());
        }
      }
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (socket_) {
      try {
        if (connect_mode_) {
          socket_->disconnect(endpoint_);
        } else {
          socket_->unbind(endpoint_);
        }
      } catch (...) {
      }
      socket_.reset();
    }
    context_.reset();
    binding_cache_.clear();
  }

 private:
  ZmqDialog dialog_;

  std::string address_ = "localhost";
  int port_ = 9872;
  std::string transport_ = "tcp://";
  bool connect_mode_ = true;
  std::string topic_filter_;
  std::string default_encoding_ = "json";
  std::string endpoint_;

  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> socket_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(ZmqSource, kZmqManifest)

PJ_DIALOG_PLUGIN(ZmqDialog)
