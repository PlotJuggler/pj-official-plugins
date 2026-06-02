// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "webrtc_signaling.hpp"

#include <mutex>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <utility>
#include <variant>
#include <vector>

namespace PJ {
namespace webrtc {

WebrtcSignaling::WebrtcSignaling() = default;

WebrtcSignaling::~WebrtcSignaling() {
  close();
}

void WebrtcSignaling::open(const SignalingConfig& config) {
  config_ = config;
  hello_done_ = false;
  ws_ = std::make_shared<rtc::WebSocket>();

  ws_->onOpen([this]() {
    // Register with the simple-signaling broker.
    if (ws_) {
      ws_->send("HELLO " + config_.our_id);
    }
  });

  ws_->onClosed([this]() {
    if (on_closed_) {
      on_closed_();
    }
  });

  ws_->onError([this](std::string /*error*/) {
    if (on_closed_) {
      on_closed_();
    }
  });

  ws_->onMessage([this](rtc::message_variant data) {
    if (std::holds_alternative<std::string>(data)) {
      onMessage(std::get<std::string>(data));
    }
  });

  try {
    ws_->open(config_.url);
  } catch (const std::exception&) {
    if (on_closed_) {
      on_closed_();
    }
  }
}

void WebrtcSignaling::close() {
  if (ws_) {
    try {
      ws_->close();
    } catch (...) {}
    ws_.reset();
  }
  hello_done_ = false;
}

void WebrtcSignaling::detachCallbacks() {
  // Drop libdatachannel's own dispatch first so no queued WS message can re-enter
  // onMessage and fire a user callback while we clear them.
  if (ws_) {
    try {
      ws_->resetCallbacks();
    } catch (...) {}
  }
  on_sdp_ = {};
  on_ice_ = {};
  on_connected_ = {};
  on_closed_ = {};
  on_error_ = {};
  on_catalog_ = {};
}

bool WebrtcSignaling::isOpen() const {
  return ws_ && ws_->isOpen();
}

void WebrtcSignaling::onMessage(const std::string& text) {
  if (!hello_done_) {
    if (text == "HELLO") {
      hello_done_ = true;
      // Pair with the peer if we were configured to initiate; otherwise we wait
      // for the offerer to SESSION to us.
      if (!config_.peer_id.empty() && ws_) {
        ws_->send("SESSION " + config_.peer_id);
      }
      if (on_connected_) {
        on_connected_();
      }
      return;
    }
    // Any non-HELLO first reply (e.g. "ERROR ...") is a failed handshake.
    if (text.rfind("ERROR", 0) == 0 && on_error_) {
      on_error_(text);
    }
    if (on_closed_) {
      on_closed_();
    }
    return;
  }

  if (text == "SESSION_OK") {
    return;  // pairing confirmed; the offer follows
  }
  if (text.rfind("ERROR", 0) == 0) {
    // Surface the reason verbatim (PROTOCOL.md §6.7) so the owner can decide
    // whether retrying is futile, then tear the session down.
    if (on_error_) {
      on_error_(text);
    }
    if (on_closed_) {
      on_closed_();
    }
    return;
  }

  auto msg = nlohmann::json::parse(text, nullptr, false);
  if (msg.is_discarded()) {
    return;
  }
  if (msg.contains("type") && msg["type"].is_string()) {
    const std::string type = msg["type"].get<std::string>();
    if (type == "catalog") {
      onCatalog(msg);
    }
    // "list"/"subscribe" are receiver->streamer only; ignore any echo.
    return;
  }
  if (msg.contains("sdp") && msg["sdp"].is_object()) {
    const auto& sdp = msg["sdp"];
    const std::string type = sdp.value("type", std::string());
    const std::string text_sdp = sdp.value("sdp", std::string());
    if (!text_sdp.empty() && on_sdp_) {
      on_sdp_(type, text_sdp);
    }
  } else if (msg.contains("ice") && msg["ice"].is_object()) {
    const auto& ice = msg["ice"];
    const std::string candidate = ice.value("candidate", std::string());
    const int mline = ice.value("sdpMLineIndex", 0);
    if (!candidate.empty() && on_ice_) {
      on_ice_(candidate, mline);
    }
  }
}

void WebrtcSignaling::onCatalog(const nlohmann::json& msg) {
  // Forward-compat: tolerate a newer protocol; we read only fields we know.
  std::vector<DiscoveredStream> streams;
  if (msg.contains("streams") && msg["streams"].is_array()) {
    for (const auto& e : msg["streams"]) {
      if (!e.is_object()) {
        continue;
      }
      DiscoveredStream s;
      s.id = e.value("id", std::string());
      if (s.id.empty()) {
        continue;  // a stream with no id is unusable
      }
      s.name = e.value("name", s.id);
      s.codec = e.value("codec", std::string("h264"));
      s.width = e.value("width", 0);
      s.height = e.value("height", 0);
      s.mid = e.value("mid", s.id);  // contract: mid == id
      streams.push_back(std::move(s));
    }
  }
  {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    discovered_streams_ = streams;
  }
  if (on_catalog_) {
    on_catalog_(std::move(streams));
  }
}

void WebrtcSignaling::requestList() {
  if (!ws_ || !ws_->isOpen()) {
    return;
  }
  nlohmann::json msg;
  msg["type"] = "list";
  msg["protocol"] = kProtocolVersion;
  ws_->send(msg.dump());
}

void WebrtcSignaling::subscribe(const std::vector<std::string>& stream_ids) {
  if (!ws_ || !ws_->isOpen() || stream_ids.empty()) {
    return;
  }
  nlohmann::json msg;
  msg["type"] = "subscribe";
  msg["protocol"] = kProtocolVersion;
  msg["streams"] = stream_ids;
  ws_->send(msg.dump());
}

std::vector<DiscoveredStream> WebrtcSignaling::discoveredStreams() const {
  std::lock_guard<std::mutex> lock(catalog_mutex_);
  return discovered_streams_;
}

void WebrtcSignaling::sendSdp(const std::string& type, const std::string& sdp) {
  if (!ws_ || !ws_->isOpen()) {
    return;
  }
  nlohmann::json msg;
  msg["sdp"]["type"] = type;
  msg["sdp"]["sdp"] = sdp;
  ws_->send(msg.dump());
}

void WebrtcSignaling::sendIce(const std::string& candidate, int mline_index) {
  if (!ws_ || !ws_->isOpen()) {
    return;
  }
  nlohmann::json msg;
  msg["ice"]["candidate"] = candidate;
  msg["ice"]["sdpMLineIndex"] = mline_index;
  ws_->send(msg.dump());
}

}  // namespace webrtc
}  // namespace PJ
