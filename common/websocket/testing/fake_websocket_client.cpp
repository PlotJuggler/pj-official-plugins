#include <pj_websocket/testing/fake_websocket_client.hpp>

namespace PJ::WebSocket {

struct Client::State {
  std::shared_ptr<testing::FakeSocket> fake;
};

namespace testing {

std::vector<std::shared_ptr<FakeSocket>>& fakeSockets() {
  static std::vector<std::shared_ptr<FakeSocket>> sockets;
  return sockets;
}

}  // namespace testing

PJ::Status Client::start(std::string url, EventSink sink, std::vector<std::string> subprotocols) {
  state_ = std::make_shared<State>();
  state_->fake = std::make_shared<testing::FakeSocket>();
  state_->fake->url = std::move(url);
  state_->fake->sink = std::move(sink);
  state_->fake->subprotocols = std::move(subprotocols);
  testing::fakeSockets().push_back(state_->fake);
  return PJ::okStatus();
}

void Client::sendText(std::string text) {
  if (isOpen()) {
    state_->fake->sent.push_back(std::move(text));
  }
}

bool Client::isOpen() const {
  return state_ && state_->fake->open;
}

void Client::stop() {
  if (!state_) {
    return;
  }
  state_->fake->stopped = true;
  state_->fake->open = false;
}

}  // namespace PJ::WebSocket
