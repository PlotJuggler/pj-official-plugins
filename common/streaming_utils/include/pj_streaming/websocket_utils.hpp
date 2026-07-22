// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ixwebsocket/IXWebSocket.h>

#include <chrono>
#include <thread>

namespace pj::streaming {

/// Start an ixwebsocket connection and wait for its asynchronous attempt to
/// finish. A newly-started socket reports Closed until its worker observes the
/// request, so Closed becomes terminal only after Connecting was observed.
[[nodiscard]] inline bool startAndWaitForOpen(
    ix::WebSocket& socket, std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_period = std::chrono::milliseconds(50)) {
  socket.start();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool attempt_started = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto state = socket.getReadyState();
    if (state == ix::ReadyState::Open) {
      return true;
    }
    if (state == ix::ReadyState::Connecting) {
      attempt_started = true;
    } else if (state == ix::ReadyState::Closed && attempt_started) {
      return false;
    }
    std::this_thread::sleep_for(poll_period);
  }
  return socket.getReadyState() == ix::ReadyState::Open;
}

}  // namespace pj::streaming
