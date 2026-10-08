// Lifetime tests for MqttDialog: the dialog must be able to drop its MQTT
// client at any moment (mid-connect, with topics still arriving) without the
// client's callbacks touching members that are already destroyed. They talk to
// a tiny in-process broker over loopback, so they are POSIX-only.
#include <gtest/gtest.h>

#ifdef _WIN32

TEST(MqttDialogLifetime, Skipped) {
  GTEST_SKIP() << "POSIX sockets";
}

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "mqtt_dialog.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SIGPIPE is disabled per socket (SO_NOSIGPIPE) in serve()
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum class BrokerMode { kFlood, kSilent };

/// Minimal MQTT 3.1.1 broker. kFlood: CONNACK, SUBACK, then QoS0 PUBLISHes as
/// fast as the socket takes them. kSilent: accepts and never answers, i.e. a
/// broker that hangs a connect.
class FakeBroker {
 public:
  explicit FakeBroker(BrokerMode mode) : mode_(mode) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    socklen_t len = sizeof(addr);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 16) != 0 ||
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      ADD_FAILURE() << "fake broker could not listen";
      return;
    }
    port_ = ntohs(addr.sin_port);
    acceptor_ = std::thread([this] { acceptLoop(); });
  }

  ~FakeBroker() {
    stop_ = true;
    if (acceptor_.joinable()) {
      acceptor_.join();
    }
    for (auto& t : connections_) {
      t.join();
    }
    ::close(listen_fd_);
  }

  FakeBroker(const FakeBroker&) = delete;
  FakeBroker& operator=(const FakeBroker&) = delete;

  [[nodiscard]] int port() const {
    return port_;
  }

  [[nodiscard]] int accepted() const {
    return accepted_;
  }

 private:
  void acceptLoop() {
    while (!stop_) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      if (::poll(&pfd, 1, 20) > 0) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd >= 0) {
          ++accepted_;
          connections_.emplace_back([this, fd] { serve(fd); });
        }
      }
    }
  }

  // Reads exactly n bytes; false on EOF/error/stop. Polls so a stop is noticed.
  bool readExact(int fd, uint8_t* out, size_t n) const {
    while (n > 0 && !stop_) {
      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, 20) <= 0) {
        continue;
      }
      const ssize_t r = ::recv(fd, out, n, 0);
      if (r <= 0) {
        return false;
      }
      out += r;
      n -= static_cast<size_t>(r);
    }
    return n == 0;
  }

  // Reads one packet: returns its fixed-header byte and fills `body`.
  bool readPacket(int fd, uint8_t& type, std::vector<uint8_t>& body) const {
    size_t remaining = 0;
    size_t shift = 0;
    uint8_t b = 0;
    if (!readExact(fd, &type, 1)) {
      return false;
    }
    do {
      if (!readExact(fd, &b, 1)) {
        return false;
      }
      remaining |= static_cast<size_t>(b & 0x7F) << shift;
      shift += 7;
    } while ((b & 0x80) != 0 && shift < 28);
    body.resize(remaining);
    return remaining == 0 || readExact(fd, body.data(), remaining);
  }

  static bool sendAll(int fd, const std::vector<uint8_t>& data) {
    return ::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size());
  }

  void serve(int fd) {
    timeval tv{0, 100000};  // a full socket buffer must not block past a stop
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (mode_ == BrokerMode::kSilent) {
      while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      ::close(fd);
      return;
    }

    uint8_t type = 0;
    std::vector<uint8_t> body;
    bool ok = readPacket(fd, type, body) && type == 0x10 && sendAll(fd, {0x20, 0x02, 0x00, 0x00});
    while (ok && !stop_) {
      ok = readPacket(fd, type, body);
      if (ok && type == 0x82 && body.size() >= 2) {
        ok = sendAll(fd, {0x90, 0x03, body[0], body[1], 0x00});
        break;
      }
    }
    for (unsigned n = 0; ok && !stop_; ++n) {
      const std::string topic = "t/" + std::to_string(n % 50);
      const std::string payload = "42";
      const auto remaining = 2 + topic.size() + payload.size();  // < 128: one-byte varint
      std::vector<uint8_t> pkt{0x30, static_cast<uint8_t>(remaining), 0, static_cast<uint8_t>(topic.size())};
      pkt.insert(pkt.end(), topic.begin(), topic.end());
      pkt.insert(pkt.end(), payload.begin(), payload.end());
      ok = sendAll(fd, pkt);
    }
    ::close(fd);
  }

  BrokerMode mode_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> accepted_{0};
  std::thread acceptor_;
  std::vector<std::thread> connections_;  // touched only by the acceptor, then the destructor
};

std::string connectButtonText(MqttDialog& dialog) {
  return nlohmann::json::parse(dialog.widget_data()).at("buttonConnect").at("button_text").get<std::string>();
}

std::size_t discoveredTopicCount(MqttDialog& dialog) {
  const auto list = nlohmann::json::parse(dialog.widget_data()).at("listWidget");
  return list.contains("list_items") ? list.at("list_items").size() : 0;
}

// Pumps onTick, as the host does, until `done` holds or a generous deadline
// (CI runners can be slow) passes.
template <typename Pred>
bool pumpUntil(MqttDialog& dialog, Pred done) {
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (Clock::now() < deadline) {
    dialog.onTick();
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

// Connected AND topics already flowing through the message callback.
bool pumpUntilTopicsArrive(MqttDialog& dialog) {
  return pumpUntil(dialog, [&] { return discoveredTopicCount(dialog) > 0; });
}

std::unique_ptr<MqttDialog> makeDialog(int port) {
  auto dialog = std::make_unique<MqttDialog>();
  dialog->onTextChanged("lineEditHost", "127.0.0.1");
  dialog->onTextChanged("lineEditPort", std::to_string(port));
  return dialog;
}

template <typename F>
std::chrono::milliseconds timed(F&& f) {
  const auto start = Clock::now();
  f();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

TEST(MqttDialogLifetime, DestroyWhileTopicsArrive) {
  FakeBroker broker(BrokerMode::kFlood);
  for (int i = 0; i < 20; ++i) {
    auto dialog = makeDialog(broker.port());
    dialog->onClicked("buttonConnect");
    ASSERT_TRUE(pumpUntilTopicsArrive(*dialog)) << "iteration " << i;
    EXPECT_LT(timed([&] { dialog.reset(); }).count(), 3000) << "iteration " << i;
  }
}

TEST(MqttDialogLifetime, CancelTwiceAgainstSilentBroker) {
  FakeBroker broker(BrokerMode::kSilent);
  auto dialog = makeDialog(broker.port());
  // The old code parked a cancelled client and waited 2 s for it on the next
  // Cancel and on close; each Cancel must now return well under that.
  for (int i = 0; i < 2; ++i) {
    dialog->onClicked("buttonConnect");
    ASSERT_EQ(connectButtonText(*dialog), "Cancel");
    // Cancel only once the broker holds the TCP connection: the connect is in flight.
    ASSERT_TRUE(pumpUntil(*dialog, [&] { return broker.accepted() > i; })) << "cancel " << i;
    EXPECT_LT(timed([&] { dialog->onClicked("buttonConnect"); }).count(), 1500) << "cancel " << i;
    EXPECT_EQ(connectButtonText(*dialog), "Connect");
  }
  EXPECT_LT(timed([&] { dialog.reset(); }).count(), 1500);
}

TEST(MqttDialogLifetime, DisconnectWhileTopicsArrive) {
  FakeBroker broker(BrokerMode::kFlood);
  auto dialog = makeDialog(broker.port());
  dialog->onClicked("buttonConnect");
  ASSERT_TRUE(pumpUntilTopicsArrive(*dialog));
  EXPECT_LT(timed([&] { dialog->onClicked("buttonConnect"); }).count(), 3000);
  EXPECT_EQ(connectButtonText(*dialog), "Connect");
  EXPECT_EQ(discoveredTopicCount(*dialog), 0u);  // the catalog belonged to the broker we left

  // The dialog is reusable after a disconnect.
  dialog->onClicked("buttonConnect");
  ASSERT_TRUE(pumpUntilTopicsArrive(*dialog));
  dialog.reset();
}

}  // namespace

#endif  // _WIN32
