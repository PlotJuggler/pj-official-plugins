// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free WHEP receive connection: ONE rtc::PeerConnection carrying ONE
// recvonly H.264 track for ONE camera path. The plugin is the OFFERER (WHEP
// contract): open() adds the track, waits for ICE gathering to complete on a
// worker thread, POSTs the offer via whep_client, applies the answer, and
// primes the Annex-B normalizer from the answer's sprop-parameter-sets.
//
// THREADING: onFrame fires on a libdatachannel worker thread; the connect
// sequence runs on this object's own worker thread (blocking HTTP must never
// run on the poll thread or a libdatachannel callback). frames_mutex_ guards
// the queue + normalizer. close() joins the worker, best-effort DELETEs the
// session, and detaches every callback; it may block up to the in-flight HTTP
// timeout (a few seconds worst case). This class never calls host methods.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "video_emit.hpp"
#include "whep_client.hpp"

namespace rtc {
class PeerConnection;
class Track;
}  // namespace rtc

namespace PJ {
namespace webrtc {

// A single ICE server entry. STUN: leave username/credential empty. TURN:
// supply both.
struct IceServerConfig {
  std::string url;
  std::string username;
  std::string credential;
};

enum class ConnectionState {
  kNew,
  kConnecting,
  kConnected,
  kDisconnected,
  kFailed,
  kClosed,
};

struct WhepConnectionConfig {
  std::string whep_url;      // <server>/<path>/whep
  std::string bearer_token;  // empty => no Authorization header
  std::vector<IceServerConfig> ice_servers;
  int h264_payload_type = 96;
  std::chrono::seconds connect_timeout{5};  // gathering wait AND each HTTP call
  std::chrono::seconds delete_timeout{2};
};

class WhepConnection {
 public:
  using StateCallback = std::function<void(ConnectionState)>;
  using ErrorCallback = std::function<void(WhepErrorKind, const std::string& reason)>;

  WhepConnection();
  ~WhepConnection();

  WhepConnection(const WhepConnection&) = delete;
  WhepConnection& operator=(const WhepConnection&) = delete;

  void setStateCallback(StateCallback cb) {
    on_state_ = std::move(cb);
  }
  // Fired once per failed connect attempt (HTTP error, gathering timeout,
  // rejected answer). The owner classifies terminal (kUnauthorized) vs retry.
  void setErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
  }

  // Build the PC + track and start the connect worker. Set callbacks BEFORE
  // open(). Idempotent teardown via close().
  [[nodiscard]] PJ::Status open(const WhepConnectionConfig& config);
  // Join worker, DELETE the session (best effort, bounded), drop callbacks,
  // close the PC, clear the queue. Safe to call twice / without open().
  void close();

  // Swap-drain the reassembled frames. onPoll() only.
  std::vector<EncodedFrame> drain();

  ConnectionState state() const {
    return state_.load();
  }

  // --- test seams (no PeerConnection involved) ---
  // Normalize one depacketized access unit with a normalizer primed from the
  // given sprop-parameter-sets (same path the live onFrame uses).
  static EncodedFrame normalizeAccessUnit(
      const uint8_t* au, size_t size, int64_t ts_ns, const std::string& sprop_parameter_sets);
  // First sprop-parameter-sets value of the first video m-line in the SDP.
  static std::string extractSpropForTest(const std::string& answer_sdp);
  // Route one AU through the live onFrame path so drain() can be asserted.
  void feedAccessUnitForTest(const uint8_t* data, size_t size) {
    onFrame(data, size);
  }
  // Prime the member normalizer as the worker would from an answer SDP.
  void primeFromAnswerForTest(const std::string& answer_sdp);

 private:
  void onFrame(const uint8_t* data, size_t size);
  void runConnect();  // worker thread body
  void reportState(ConnectionState s);
  static int64_t wallClockNs();

  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> track_;  // RETAIN: libdatachannel keeps only a weak ref
  WhepConnectionConfig config_;

  std::mutex frames_mutex_;
  H264AnnexBNormalizer normalizer_;
  std::queue<EncodedFrame> queue_;

  std::thread worker_;
  std::mutex gather_mutex_;
  std::condition_variable gather_cv_;
  bool gathering_done_ = false;
  bool closing_ = false;

  std::mutex session_mutex_;
  std::string session_url_;

  std::atomic<ConnectionState> state_{ConnectionState::kNew};
  StateCallback on_state_;
  ErrorCallback on_error_;
};

}  // namespace webrtc
}  // namespace PJ
