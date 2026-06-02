// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free WebRTC signaling client. libdatachannel does no signaling, so this
// exchanges SDP/ICE over an rtc::WebSocket using the GStreamer webrtc_sendrecv
// "simple signaling" protocol so the demo/ GStreamer rig interoperates with no
// glue. The plugin is the ANSWERER: the GStreamer pipeline offers.
//
//   connect -> "HELLO <our_id>"   (server replies "HELLO")
//   -> wait for the peer's offer relayed as {"sdp":{type,sdp}}
//   -> reply {"sdp":{type,sdp}} / {"ice":{candidate,sdpMLineIndex}}
//
// All callbacks fire on a libdatachannel worker thread; keep them non-blocking.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rtc {
class WebSocket;
}  // namespace rtc

namespace PJ {
namespace webrtc {

struct SignalingConfig {
  std::string url;      // assembled ws:// URL (ws://<address>:<port>)
  std::string our_id;   // HELLO id this plugin registers as
  std::string peer_id;  // optional: peer to SESSION to (empty => plugin waits)
};

// One advertised camera/stream from a streamer's "catalog" message.
// `id == mid` is the contract (PROTOCOL.md §2); `mid` defaults to `id`.
struct DiscoveredStream {
  std::string id;
  std::string name;
  std::string codec = "h264";
  int width = 0;
  int height = 0;
  std::string mid;
};

class WebrtcSignaling {
 public:
  using SdpCallback = std::function<void(const std::string& type, const std::string& sdp)>;
  using IceCallback = std::function<void(const std::string& candidate, int mline_index)>;
  using StateCallback = std::function<void()>;

  WebrtcSignaling();
  ~WebrtcSignaling();

  WebrtcSignaling(const WebrtcSignaling&) = delete;
  WebrtcSignaling& operator=(const WebrtcSignaling&) = delete;

  void setSdpCallback(SdpCallback cb) {
    on_sdp_ = std::move(cb);
  }
  void setIceCallback(IceCallback cb) {
    on_ice_ = std::move(cb);
  }
  void setConnectedCallback(StateCallback cb) {
    on_connected_ = std::move(cb);
  }
  void setClosedCallback(StateCallback cb) {
    on_closed_ = std::move(cb);
  }

  using CatalogCallback = std::function<void(std::vector<DiscoveredStream>)>;
  void setCatalogCallback(CatalogCallback cb) {
    on_catalog_ = std::move(cb);
  }

  // Ask the paired streamer/broker to advertise. No-op if not open.
  void requestList();
  // Subscribe to the chosen streams (non-empty). The streamer answers with ONE
  // multi-m-line offer (a=mid:<id> per stream).
  void subscribe(const std::vector<std::string>& stream_ids);
  // Thread-safe snapshot of the last catalog parsed.
  std::vector<DiscoveredStream> discoveredStreams() const;

  // Open the WebSocket and start the HELLO/SESSION handshake. Non-blocking;
  // completion arrives via callbacks. Idempotent teardown via close().
  void open(const SignalingConfig& config);
  void close();

  bool isOpen() const;

  // Send our local description / candidate to the paired peer (verbatim relay).
  void sendSdp(const std::string& type, const std::string& sdp);
  void sendIce(const std::string& candidate, int mline_index);

 private:
  void onMessage(const std::string& text);
  void onCatalog(const nlohmann::json& msg);

  static constexpr int kProtocolVersion = 1;

  std::shared_ptr<rtc::WebSocket> ws_;
  SignalingConfig config_;
  bool hello_done_ = false;

  SdpCallback on_sdp_;
  IceCallback on_ice_;
  StateCallback on_connected_;
  StateCallback on_closed_;

  CatalogCallback on_catalog_;
  mutable std::mutex catalog_mutex_;
  std::vector<DiscoveredStream> discovered_streams_;
};

}  // namespace webrtc
}  // namespace PJ
