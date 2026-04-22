#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/encoding_utils.hpp>

#include "udp_dialog.hpp"
#include "udp_manifest.hpp"

#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// Forward declaration of the dialog vtable emitter — defined at file scope
// by PJ_DIALOG_PLUGIN(UdpDialog) at the bottom of this TU.
extern "C" PJ_DIALOG_EXPORT const PJ_dialog_vtable_t* PJ_get_dialog_vtable() noexcept;

namespace {

constexpr size_t kMaxDatagramSize = 65507;  // max UDP payload (IPv4)

class UdpSource : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ_borrowed_dialog_t{&dialog_, PJ_get_dialog_vtable()};
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.setAvailableEncodings(
        PJ::sdk::parseEncodingsJson(runtimeHost().listAvailableEncodings()));

    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);
    }

    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }

    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = static_cast<uint16_t>(cfg.value("port", 9870));
    default_encoding_ = cfg.value("default_encoding", std::string("json"));

    asio::error_code ec;

    auto addr = asio::ip::make_address(address_, ec);
    if (ec) {
      return PJ::unexpected("invalid address '" + address_ + "': " + ec.message());
    }

    bool is_multicast = addr.is_multicast();
    auto protocol = addr.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4();

    io_context_ = std::make_unique<asio::io_context>();
    socket_ = std::make_unique<asio::ip::udp::socket>(*io_context_);

    socket_->open(protocol, ec);
    if (ec) {
      return PJ::unexpected("socket open failed: " + ec.message());
    }

    socket_->set_option(asio::socket_base::reuse_address(true), ec);

    if (is_multicast) {
      // Multicast: bind to any, then join the group.
      auto bind_ep = addr.is_v6()
                         ? asio::ip::udp::endpoint(asio::ip::udp::v6(), port_)
                         : asio::ip::udp::endpoint(asio::ip::udp::v4(), port_);
      socket_->bind(bind_ep, ec);
      if (ec) {
        return PJ::unexpected("UDP bind failed: " + ec.message());
      }
      socket_->set_option(asio::ip::multicast::join_group(addr), ec);
      if (ec) {
        return PJ::unexpected("multicast join failed: " + ec.message());
      }
    } else {
      // Unicast / broadcast
      asio::ip::udp::endpoint endpoint(addr, port_);
      socket_->bind(endpoint, ec);
      if (ec) {
        return PJ::unexpected("UDP bind failed: " + ec.message());
      }
    }

    socket_->non_blocking(true, ec);
    if (ec) {
      return PJ::unexpected("non_blocking failed: " + ec.message());
    }

    binding_cache_.clear();
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    constexpr int kMaxDatagramsPerPoll = 200;

    for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
      asio::ip::udp::endpoint sender;
      asio::error_code ec;

      size_t n = socket_->receive_from(asio::buffer(recv_buffer_), sender, 0, ec);

      if (ec == asio::error::would_block) {
        break;  // no more datagrams pending
      }
      if (ec || n == 0) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                   "receive_from error: " + ec.message());
        break;
      }

      auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
      int64_t timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

      auto it = binding_cache_.find(default_encoding_);
      if (it == binding_cache_.end()) {
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = "udp/data",
            .parser_encoding = default_encoding_,
            .type_name = {},
            .schema = {},
            .parser_config_json = {},
        });
        if (binding) {
          it = binding_cache_.emplace(default_encoding_, *binding).first;
        }
      }

      if (it != binding_cache_.end()) {
        auto status = runtimeHost().pushRawMessage(
            it->second, PJ::Timestamp{timestamp_ns},
            PJ::Span<const uint8_t>(recv_buffer_.data(), n));
        if (!status) {
          // Mirror PJ 3.x behavior: a parse failure stops the stream.
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError,
                                     "Parse error — stopping stream: " + status.error());
          return PJ::unexpected(status.error());
        }
      }
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (socket_) {
      asio::error_code ec;
      socket_->close(ec);
      socket_.reset();
    }
    io_context_.reset();
    binding_cache_.clear();
  }

 private:
  UdpDialog dialog_;

  std::string address_ = "127.0.0.1";
  uint16_t port_ = 9870;
  std::string default_encoding_ = "json";

  std::unique_ptr<asio::io_context> io_context_;
  std::unique_ptr<asio::ip::udp::socket> socket_;
  std::array<uint8_t, kMaxDatagramSize> recv_buffer_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(UdpSource, kUdpManifest)

PJ_DIALOG_PLUGIN(UdpDialog)
