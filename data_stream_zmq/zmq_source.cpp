#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/number_parse.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/encoding_utils.hpp>
#include <pj_plugins/sdk/endpoint.hpp>
#include <pj_plugins/sdk/streaming_source.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <zmq.hpp>

#include "zmq_dialog.hpp"
#include "zmq_manifest.hpp"

namespace {

class ZmqSource : public PJ::StreamSourceBase {
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
    // Encodings are needed even when the config is empty (first run).
    dialog_.setAvailableEncodings(PJ::sdk::parseEncodingsJson(runtimeHost().listAvailableEncodings()));

    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);  // errors keep the defaults
    }

    // The parser configuration the app injects under "_parser_config" (e.g. the
    // protobuf descriptor + message type) must reach ensureParserBinding, or
    // schema-based parsers bind with no schema and drop every message.
    parser_config_override_ = PJ::sdk::parserConfigOverride(config_json);

    return PJ::okStatus();
  }

  PJ::Status onStart() override {
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

    endpoint_ = transport_ == "ipc://" ? transport_ + address_
                                       : PJ::sdk::composeEndpoint(transport_, address_, static_cast<uint16_t>(port_));

    try {
      if (connect_mode_) {
        socket_->connect(endpoint_);
      } else {
        socket_->bind(endpoint_);
      }

      if (topic_filter_.empty()) {
        socket_->set(zmq::sockopt::subscribe, "");
      } else {
        // Separators: comma, semicolon, space, tab.
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
      if (!result || recv_msg.size() == 0) {
        break;
      }

      // A multi-part message starts with the topic frame.
      std::string topic;
      if (recv_msg.more()) {
        topic = std::string(static_cast<const char*>(recv_msg.data()), recv_msg.size());
        recv_msg.rebuild();
        result = socket_->recv(recv_msg, zmq::recv_flags::dontwait);
        if (!result || recv_msg.size() == 0) {
          continue;
        }
      }

      auto* payload_data = static_cast<const uint8_t*>(recv_msg.data());
      size_t payload_size = recv_msg.size();

      // Optional third frame: timestamp as text (seconds since epoch)
      int64_t timestamp_ns = 0;
      bool timestamp_parsed = false;
      if (recv_msg.more()) {
        zmq::message_t ts_msg;
        result = socket_->recv(ts_msg, zmq::recv_flags::dontwait);
        if (result && ts_msg.size() > 0) {
          std::string_view ts_str(static_cast<const char*>(ts_msg.data()), ts_msg.size());
          while (!ts_str.empty() && std::isspace(static_cast<unsigned char>(ts_str.front()))) {
            ts_str.remove_prefix(1);
          }
          while (!ts_str.empty() && std::isspace(static_cast<unsigned char>(ts_str.back()))) {
            ts_str.remove_suffix(1);
          }
          // Locale-independent: std::strtod respects LC_NUMERIC and under a
          // comma-decimal locale truncates "1721.5" to 1721.
          if (auto ts_sec = PJ::parseNumber<double>(ts_str)) {
            timestamp_ns = static_cast<int64_t>(*ts_sec * 1e9);
            timestamp_parsed = true;
          }
        }
        // Discard any further frames.
        while (ts_msg.more()) {
          ts_msg.rebuild();
          (void)socket_->recv(ts_msg, zmq::recv_flags::dontwait);
        }
      }
      if (!timestamp_parsed) {
        // No timestamp frame, or one that is not a number: use arrival time (an
        // unparsable frame must not become timestamp 0, which mis-places the
        // sample by decades).
        auto now = std::chrono::system_clock::now().time_since_epoch();
        timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
      }

      std::string topic_name = topic.empty() ? "zmq/data" : topic;

      auto status = ingest_.push(
          runtimeHost(), topic_name,
          {
              .topic_name = topic_name,
              .parser_encoding = default_encoding_,
              .type_name = {},
              .schema = {},
              .parser_config_json = parser_config_override_,
          },
          PJ::Timestamp{timestamp_ns}, std::vector<uint8_t>(payload_data, payload_data + payload_size));
      if (!status) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, "Failed to push message: " + status.error());
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
      } catch (...) {}
      socket_.reset();
    }
    context_.reset();
    ingest_.clear();
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
  std::string parser_config_override_;

  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> socket_;
  PJ::sdk::DelegatedIngestCache ingest_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(ZmqSource, kZmqManifest)

PJ_DIALOG_PLUGIN(ZmqDialog, kZmqManifest)
