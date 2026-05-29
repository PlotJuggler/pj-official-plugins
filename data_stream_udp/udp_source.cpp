#include <algorithm>
#include <array>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/encoding_utils.hpp>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "udp_dialog.hpp"
#include "udp_manifest.hpp"

namespace {

constexpr size_t kMaxDatagramSize = 65507;  // max UDP payload (IPv4)

// A datagram captured by the shared receiver and fanned out to each subscriber.
struct PendingDatagram {
  std::vector<uint8_t> data;
  int64_t timestamp_ns;
};

// Per-stream inbox. Each UdpSource owns one; the shared endpoint copies every
// received datagram into every inbox so concurrent streams all see all data.
struct UdpSubscriber {
  std::mutex mutex;
  std::queue<PendingDatagram> queue;
};

// One real UDP socket per "address:port", shared by every UdpSource bound to
// that endpoint. Linux delivers each unicast datagram to a single socket, so
// two independent sockets on the same port would starve each other — the older
// stream silently stops receiving (it "freezes"). Instead we keep a single
// socket and fan each datagram out to all registered subscribers. This lets two
// streams on the same port coexist, e.g. one parsing with embedded timestamps
// and one without, both seeing the full data — the UDP analogue of giving each
// MQTT stream a distinct client ID.
class SharedUdpEndpoint : public std::enable_shared_from_this<SharedUdpEndpoint> {
 public:
  ~SharedUdpEndpoint() {
    stopped_ = true;
    if (socket_) {
      asio::error_code ec;
      socket_->close(ec);
    }
    if (io_context_) {
      io_context_->stop();
    }
    if (io_thread_.joinable()) {
      io_thread_.join();
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& reg = registry();
    if (auto it = reg.find(key_); it != reg.end() && it->second.expired()) {
      reg.erase(it);
    }
  }

  // Create-or-get the shared endpoint for address:port. The first caller opens
  // and binds the socket and starts the background io thread; later callers for
  // the same endpoint reuse it.
  static PJ::Expected<std::shared_ptr<SharedUdpEndpoint>> acquire(const std::string& address, uint16_t port) {
    const std::string key = address + ":" + std::to_string(port);
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& reg = registry();
    if (auto it = reg.find(key); it != reg.end()) {
      if (auto existing = it->second.lock()) {
        return existing;
      }
    }
    std::shared_ptr<SharedUdpEndpoint> ep(new SharedUdpEndpoint());
    if (auto status = ep->open(address, port); !status) {
      return PJ::unexpected(status.error());
    }
    ep->key_ = key;
    reg[key] = ep;
    return ep;
  }

  std::shared_ptr<UdpSubscriber> subscribe() {
    auto sub = std::make_shared<UdpSubscriber>();
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.push_back(sub);
    return sub;
  }

  void unsubscribe(const std::shared_ptr<UdpSubscriber>& sub) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), sub), subscribers_.end());
  }

 private:
  SharedUdpEndpoint() = default;

  static std::mutex& registryMutex() {
    static std::mutex m;
    return m;
  }
  static std::map<std::string, std::weak_ptr<SharedUdpEndpoint>>& registry() {
    static std::map<std::string, std::weak_ptr<SharedUdpEndpoint>> r;
    return r;
  }

  PJ::Status open(const std::string& address, uint16_t port) {
    asio::error_code ec;

    auto addr = asio::ip::make_address(address, ec);
    if (ec) {
      return PJ::unexpected("invalid address '" + address + "': " + ec.message());
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
      auto bind_ep = addr.is_v6() ? asio::ip::udp::endpoint(asio::ip::udp::v6(), port)
                                  : asio::ip::udp::endpoint(asio::ip::udp::v4(), port);
      socket_->bind(bind_ep, ec);
      if (ec) {
        return PJ::unexpected("UDP bind failed: " + ec.message());
      }
      socket_->set_option(asio::ip::multicast::join_group(addr), ec);
      if (ec) {
        return PJ::unexpected("multicast join failed: " + ec.message());
      }
    } else {
      asio::ip::udp::endpoint endpoint(addr, port);
      socket_->bind(endpoint, ec);
      if (ec) {
        return PJ::unexpected("UDP bind failed: " + ec.message());
      }
    }

    startAsyncReceive();
    io_thread_ = std::thread([this] { io_context_->run(); });
    return PJ::okStatus();
  }

  // Async receive chain on io_thread_. Each completion handler fans the datagram
  // out to every subscriber's queue and immediately rearms.
  void startAsyncReceive() {
    if (stopped_) {
      return;
    }
    socket_->async_receive_from(
        asio::buffer(recv_buffer_), sender_endpoint_, [this](asio::error_code ec, std::size_t n) {
          if (!ec && n > 0) {
            const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
            const int64_t ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

            std::lock_guard<std::mutex> lock(subscribers_mutex_);
            for (auto& sub : subscribers_) {
              std::lock_guard<std::mutex> qlock(sub->mutex);
              sub->queue.push({std::vector<uint8_t>(recv_buffer_.data(), recv_buffer_.data() + n), ts_ns});
            }
          }
          startAsyncReceive();  // rearm regardless of error (socket closed → exits)
        });
  }

  std::unique_ptr<asio::io_context> io_context_;
  std::unique_ptr<asio::ip::udp::socket> socket_;
  std::array<uint8_t, kMaxDatagramSize> recv_buffer_;
  asio::ip::udp::endpoint sender_endpoint_;
  std::thread io_thread_;
  std::atomic<bool> stopped_{false};

  std::mutex subscribers_mutex_;
  std::vector<std::shared_ptr<UdpSubscriber>> subscribers_;

  std::string key_;
};

class UdpSource : public PJ::StreamSourceBase {
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
    dialog_.setAvailableEncodings(PJ::sdk::parseEncodingsJson(runtimeHost().listAvailableEncodings()));

    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);
    }

    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded() && cfg.contains("_parser_config")) {
      parser_config_override_ = cfg["_parser_config"].get<std::string>();
    } else {
      parser_config_override_.clear();
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

    auto endpoint = SharedUdpEndpoint::acquire(address_, port_);
    if (!endpoint) {
      return PJ::unexpected(endpoint.error());
    }
    endpoint_ = *endpoint;
    subscriber_ = endpoint_->subscribe();

    binding_cache_.clear();
    return PJ::okStatus();
  }

  // onPoll() drains this stream's own inbox — never touches the network.
  PJ::Status onPoll() override {
    std::queue<PendingDatagram> batch;
    {
      std::lock_guard<std::mutex> lock(subscriber_->mutex);
      std::swap(batch, subscriber_->queue);
    }

    while (!batch.empty()) {
      auto& dgram = batch.front();

      auto it = binding_cache_.find(default_encoding_);
      if (it == binding_cache_.end()) {
        auto binding = runtimeHost().ensureParserBinding({
            .topic_name = "udp/data",
            .parser_encoding = default_encoding_,
            .type_name = {},
            .schema = {},
            .parser_config_json = parser_config_override_,
        });
        if (binding) {
          it = binding_cache_.emplace(default_encoding_, *binding).first;
        }
      }

      if (it != binding_cache_.end()) {
        // Move the per-datagram bytes into a shared_ptr-owned buffer so the
        // PayloadView fetcher remains valid after onPoll returns
        // (ObjectIngestPolicy may defer dispatch beyond this call).
        auto payload = std::make_shared<std::vector<uint8_t>>(std::move(dgram.data));
        auto status = runtimeHost().pushMessage(
            it->second, PJ::Timestamp{dgram.timestamp_ns},
            [payload]() -> PJ::sdk::PayloadView { return PJ::sdk::PayloadView{payload}; });
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kError, "Parse error — stopping stream: " + status.error());
          return PJ::unexpected(status.error());
        }
      }

      batch.pop();
    }

    return PJ::okStatus();
  }

  void onStop() override {
    if (endpoint_ && subscriber_) {
      endpoint_->unsubscribe(subscriber_);
    }
    subscriber_.reset();
    endpoint_.reset();  // last ref → SharedUdpEndpoint tears down socket + io thread
    binding_cache_.clear();
  }

 private:
  UdpDialog dialog_;

  std::string address_ = "127.0.0.1";
  uint16_t port_ = 9870;
  std::string default_encoding_ = "json";
  std::string parser_config_override_;

  std::shared_ptr<SharedUdpEndpoint> endpoint_;
  std::shared_ptr<UdpSubscriber> subscriber_;
  std::unordered_map<std::string, PJ::ParserBindingHandle> binding_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(UdpSource, kUdpManifest)

PJ_DIALOG_PLUGIN(UdpDialog)
