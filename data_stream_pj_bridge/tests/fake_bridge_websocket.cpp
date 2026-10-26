#include "fake_bridge_websocket.hpp"

namespace PJ::BridgeProtocol {

struct BridgeWebSocket::State {
  std::shared_ptr<testing::FakeSocket> fake;
};

namespace testing {

std::vector<std::shared_ptr<FakeSocket>>& fakeSockets() {
  static std::vector<std::shared_ptr<FakeSocket>> sockets;
  return sockets;
}

}  // namespace testing

PJ::Status BridgeWebSocket::start(std::string url, EventSink sink) {
  state_ = std::make_shared<State>();
  state_->fake = std::make_shared<testing::FakeSocket>();
  state_->fake->url = std::move(url);
  state_->fake->sink = std::move(sink);
  testing::fakeSockets().push_back(state_->fake);
  return PJ::okStatus();
}

void BridgeWebSocket::sendText(std::string text) {
  if (isOpen()) {
    state_->fake->sent.push_back(std::move(text));
  }
}

bool BridgeWebSocket::isOpen() const {
  return state_ && state_->fake->open;
}

void BridgeWebSocket::stop() {
  if (!state_) {
    return;
  }
  state_->fake->stopped = true;
  state_->fake->open = false;
}

}  // namespace PJ::BridgeProtocol
