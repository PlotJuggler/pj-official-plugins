// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free WebRTC receive front-end. Owns ONE rtc::PeerConnection that carries N
// recvonly H.264 video tracks (multi-camera). Each remote offer m-line fires
// onTrack once; tracks are keyed by mid (the camera/stream id — the protocol
// contract). Per track we keep a retained rtc::Track, a primed Annex-B
// normalizer, and a frame queue. The owning StreamSourceBase swap-drains all
// tracks (tagged by stream id) from onPoll().
//
// THREADING: every callback (onTrack, per-track onFrame, onStateChange,
// onLocalDescription, onLocalCandidate) fires on a libdatachannel worker
// thread, and with N tracks multiple onFrames run concurrently. A single
// tracks_mutex_ guards the whole track map and every TrackState. This class
// never calls host methods; the source marshals to the poll thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "video_emit.hpp"

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

struct WebrtcConfig {
  std::vector<IceServerConfig> ice_servers;
  int h264_payload_type = 96;
  std::string frame_id = "webrtc";  // FALLBACK frame_id (manual/legacy 1-stream)
};

// One stream the source subscribed to. stream_id == the mid the streamer will
// assign on its m-line (the contract). frame_id is this camera's TF frame.
struct StreamSpec {
  std::string stream_id;
  std::string frame_id;
};

enum class ConnectionState {
  kNew,
  kConnecting,
  kConnected,
  kDisconnected,
  kFailed,
  kClosed,
};

// Per-track receive state. One per remote video m-line; map key == mid.
// All members touched ONLY under WebrtcReceiver::tracks_mutex_.
struct TrackState {
  // RETAIN the track: libdatachannel holds only a weak ref. Dropping this
  // shared_ptr destroys the track and rejects that m-line's media (no video).
  std::shared_ptr<rtc::Track> track;
  // Per-track normalizer: each m-line carries its OWN sprop-parameter-sets, so
  // each track injects ITS OWN SPS/PPS on keyframes. Never share across tracks.
  H264AnnexBNormalizer normalizer;
  std::queue<EncodedFrame> queue;  // this track's reassembled access units
  std::string stream_id;           // == mid (the contract); the camera id
  std::string frame_id;            // per-camera frame_id for the VideoFrame
};

class WebrtcReceiver {
 public:
  using LocalDescriptionCallback = std::function<void(const std::string& type, const std::string& sdp)>;
  using LocalCandidateCallback = std::function<void(const std::string& candidate, int mline_index)>;
  using StateCallback = std::function<void(ConnectionState)>;
  using ErrorCallback = std::function<void(const std::string& reason)>;

  WebrtcReceiver();
  ~WebrtcReceiver();

  WebrtcReceiver(const WebrtcReceiver&) = delete;
  WebrtcReceiver& operator=(const WebrtcReceiver&) = delete;

  void setLocalDescriptionCallback(LocalDescriptionCallback cb) {
    on_local_description_ = std::move(cb);
  }
  void setLocalCandidateCallback(LocalCandidateCallback cb) {
    on_local_candidate_ = std::move(cb);
  }
  void setStateCallback(StateCallback cb) {
    on_state_ = std::move(cb);
  }
  // Fired when applying a remote description throws (malformed/incompatible
  // offer). The PeerConnection never reaches Failed in that case, so without
  // this the stall is silent; the owner surfaces it and arms a reconnect.
  void setErrorCallback(ErrorCallback cb) {
    on_error_ = std::move(cb);
  }

  // Build the PeerConnection (no track added: ANSWERER role; tracks arrive from
  // the remote offer's m-lines). `expected` declares the subscribed streams so
  // frames are tagged with the right frame_id and unrequested mids are dropped.
  // Pass an empty `expected` for the manual/legacy single-stream fallback (then
  // any arriving track is accepted; its mid becomes the stream_id and
  // config.frame_id is used). Idempotent teardown via close().
  PJ::Status open(const WebrtcConfig& config, const std::vector<StreamSpec>& expected);
  void close();

  // Drop every libdatachannel callback (onTrack/onFrame/onStateChange/
  // onLocalDescription/onLocalCandidate) so no worker-thread delivery can reach
  // this object again. Call before the owner resets cross-wired peers.
  void detachCallbacks();

  // --- driven by signaling (answerer role) ---
  void setRemoteDescription(const std::string& type, const std::string& sdp);
  void addRemoteCandidate(const std::string& candidate, int mline_index);

  // Swap-drain every track's frames, tagged by stream_id. onPoll() only.
  std::vector<std::pair<std::string, EncodedFrame>> drainByStream();

  ConnectionState state() const {
    return state_.load();
  }

  // Test seam: normalize one depacketized access unit using a normalizer primed
  // from the given sprop-parameter-sets.
  static EncodedFrame normalizeAccessUnit(
      const uint8_t* au, size_t size, int64_t ts_ns, const std::string& sprop_parameter_sets);

  // Test seams (no PeerConnection): exercise the per-mid demux and the
  // unsubscribed-mid drop that onTrack/onFrame implement on the live path.
  // acceptTrackForTest mirrors onTrack's accept/drop decision WITHOUT a real
  // rtc::Track (it does not retain a track, only the receive state); it returns
  // true if the mid was accepted (subscribed, or accept-any when expected empty)
  // and false if dropped. feedAccessUnitForTest routes a depacketized access
  // unit through the same onFrame path so drainByStream tagging can be asserted.
  bool acceptTrackForTest(const std::string& mid);
  void feedAccessUnitForTest(const std::string& mid, const uint8_t* data, size_t size, int64_t ts_ns) {
    onFrame(mid, data, size, /*rtp_timestamp_90khz=*/0);
    (void)ts_ns;  // onFrame stamps with the wall clock; ts_ns is informational
  }

  // Test seams over the SDP-parsing helpers (otherwise internal-linkage in the
  // .cpp), driving the ICE-candidate->mid mapping (§6.6) and per-mid SPS/PPS
  // priming (§5.2). Pure functions: no PeerConnection involved.
  static std::vector<std::string> extractMidsInOrderForTest(const std::string& sdp);
  static std::map<std::string, std::string> extractSpropPerMidForTest(const std::string& sdp);

 private:
  void onFrame(const std::string& mid, const uint8_t* data, size_t size, uint32_t rtp_timestamp_90khz);
  void primeFromRemoteSdp(const std::string& sdp);  // primes every matching track
  static int64_t wallClockNs();

  std::shared_ptr<rtc::PeerConnection> pc_;
  WebrtcConfig config_;

  std::mutex tracks_mutex_;
  std::map<std::string, TrackState> tracks_;  // key = mid (== stream_id)
  std::vector<std::string> mid_by_mline_;     // index i -> mid of the i-th m-line
  bool expected_empty_ = true;

  std::atomic<ConnectionState> state_{ConnectionState::kNew};

  LocalDescriptionCallback on_local_description_;
  LocalCandidateCallback on_local_candidate_;
  StateCallback on_state_;
  ErrorCallback on_error_;
};

}  // namespace webrtc
}  // namespace PJ
