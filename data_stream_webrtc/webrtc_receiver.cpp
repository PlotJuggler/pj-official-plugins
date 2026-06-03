// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "webrtc_receiver.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <rtc/rtc.hpp>
#include <utility>
#include <variant>
#include <vector>

namespace PJ {
namespace webrtc {
namespace {

// First "sprop-parameter-sets=<b64,b64>" value in the given text (one SDP
// section). Empty if absent.
std::string extractSpropFromSection(const std::string& sdp) {
  const std::string key = "sprop-parameter-sets=";
  const size_t kpos = sdp.find(key);
  if (kpos == std::string::npos) {
    return {};
  }
  const size_t value_start = kpos + key.size();
  size_t value_end = value_start;
  while (value_end < sdp.size() && sdp[value_end] != ';' && sdp[value_end] != '\r' && sdp[value_end] != '\n' &&
         sdp[value_end] != ' ') {
    ++value_end;
  }
  return sdp.substr(value_start, value_end - value_start);
}

// First "a=mid:" token in the given text (one SDP section). Empty if none.
std::string extractFirstMidFromSdp(const std::string& sdp) {
  const std::string key = "a=mid:";
  const size_t kpos = sdp.find(key);
  if (kpos == std::string::npos) {
    return {};
  }
  size_t s = kpos + key.size();
  size_t e = s;
  while (e < sdp.size() && sdp[e] != '\r' && sdp[e] != '\n' && sdp[e] != ' ') {
    ++e;
  }
  return sdp.substr(s, e - s);
}

// mid -> sprop for every video m-line that carries both.
std::map<std::string, std::string> extractSpropPerMid(const std::string& sdp) {
  std::map<std::string, std::string> out;
  size_t mpos = sdp.find("m=");
  while (mpos != std::string::npos) {
    const size_t next = sdp.find("\nm=", mpos + 2);
    const std::string section = sdp.substr(mpos, next == std::string::npos ? std::string::npos : next - mpos);
    mpos = (next == std::string::npos) ? next : next + 1;  // step past '\n'
    if (section.compare(0, 7, "m=video") != 0) {
      continue;
    }
    const std::string mid = extractFirstMidFromSdp(section);
    const std::string sprop = extractSpropFromSection(section);
    if (!mid.empty() && !sprop.empty()) {
      out[mid] = sprop;
    }
  }
  return out;
}

// mid of each m-line in SDP order (video and non-video alike, so the index
// matches sdpMLineIndex which counts ALL m-lines). "" for a section with no mid.
std::vector<std::string> extractMidsInOrder(const std::string& sdp) {
  std::vector<std::string> mids;
  size_t mpos = sdp.find("m=");
  while (mpos != std::string::npos) {
    const size_t next = sdp.find("\nm=", mpos + 2);
    const std::string section = sdp.substr(mpos, next == std::string::npos ? std::string::npos : next - mpos);
    mpos = (next == std::string::npos) ? next : next + 1;
    mids.push_back(extractFirstMidFromSdp(section));
  }
  return mids;
}

ConnectionState toConnectionState(rtc::PeerConnection::State s) {
  switch (s) {
    case rtc::PeerConnection::State::New:
      return ConnectionState::kNew;
    case rtc::PeerConnection::State::Connecting:
      return ConnectionState::kConnecting;
    case rtc::PeerConnection::State::Connected:
      return ConnectionState::kConnected;
    case rtc::PeerConnection::State::Disconnected:
      return ConnectionState::kDisconnected;
    case rtc::PeerConnection::State::Failed:
      return ConnectionState::kFailed;
    case rtc::PeerConnection::State::Closed:
      return ConnectionState::kClosed;
  }
  return ConnectionState::kNew;
}

}  // namespace

WebrtcReceiver::WebrtcReceiver() = default;

WebrtcReceiver::~WebrtcReceiver() {
  close();
}

int64_t WebrtcReceiver::wallClockNs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

PJ::Status WebrtcReceiver::open(const WebrtcConfig& config, const std::vector<StreamSpec>& expected) {
  config_ = config;
  expected_empty_ = expected.empty();
  {
    std::lock_guard<std::mutex> lk(tracks_mutex_);
    tracks_.clear();
    mid_by_mline_.clear();
    for (const auto& s : expected) {
      TrackState ts;
      ts.stream_id = s.stream_id;
      ts.frame_id = s.frame_id.empty() ? config_.frame_id : s.frame_id;
      tracks_.emplace(s.stream_id, std::move(ts));  // key by mid == stream_id
    }
  }

  rtc::Configuration rtc_config;
  for (const auto& srv : config_.ice_servers) {
    if (srv.url.empty()) {
      continue;
    }
    try {
      if (!srv.username.empty() || !srv.credential.empty()) {
        rtc::IceServer parsed(srv.url);
        parsed.username = srv.username;
        parsed.password = srv.credential;
        rtc_config.iceServers.push_back(std::move(parsed));
      } else {
        rtc_config.iceServers.emplace_back(srv.url);
      }
    } catch (const std::exception&) {}
  }

  try {
    pc_ = std::make_shared<rtc::PeerConnection>(rtc_config);
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("PeerConnection creation failed: ") + e.what());
  }

  pc_->onStateChange([this](rtc::PeerConnection::State s) {
    const ConnectionState cs = toConnectionState(s);
    state_.store(cs);
    if (on_state_) {
      on_state_(cs);
    }
  });

  pc_->onLocalDescription([this](rtc::Description desc) {
    // Per-mid normalizers are primed once, from setRemoteDescription, when the
    // remote offer is applied. Re-priming here (same remote SDP) would re-parse
    // and re-decode it for no effect, since prime() is idempotent state.
    if (on_local_description_) {
      on_local_description_(desc.typeString(), std::string(desc));
    }
  });

  pc_->onLocalCandidate([this](rtc::Candidate cand) {
    if (on_local_candidate_) {
      on_local_candidate_(std::string(cand), 0);
    }
  });

  // ANSWERER: do NOT add a track. Accept the tracks libdatachannel mints from
  // the remote offer's m-lines; key each by its mid.
  pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
    const std::string mid = track->mid();  // == the offer's a=mid (the contract)

    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
    depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(depacketizer);

    // Capture the mid BY VALUE; re-find under the lock in onFrame.
    track->onFrame([this, mid](rtc::binary frame, rtc::FrameInfo info) {
      onFrame(mid, reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), info.timestamp);
    });

    std::lock_guard<std::mutex> lk(tracks_mutex_);
    auto it = tracks_.find(mid);
    if (it == tracks_.end()) {
      if (!expected_empty_) {
        // A mid we never subscribed to: drop it (do not retain).
        return;
      }
      TrackState ts;
      ts.stream_id = mid;
      ts.frame_id = config_.frame_id;  // fallback frame_id (manual/legacy)
      it = tracks_.emplace(mid, std::move(ts)).first;
    }
    it->second.track = std::move(track);  // RETAIN (per-track strong ref)
  });

  return PJ::okStatus();
}

void WebrtcReceiver::detachCallbacks() {
  // Drop libdatachannel's own dispatch (onTrack/onFrame/onStateChange/
  // onLocalDescription/onLocalCandidate) AND our user callbacks, so no
  // worker-thread delivery can call back into this object or its peers.
  if (pc_) {
    try {
      pc_->resetCallbacks();
    } catch (...) {}
  }
  on_local_description_ = {};
  on_local_candidate_ = {};
  on_state_ = {};
  on_error_ = {};
}

void WebrtcReceiver::close() {
  if (pc_) {
    try {
      pc_->close();
    } catch (...) {}
    pc_.reset();
  }
  std::lock_guard<std::mutex> lk(tracks_mutex_);
  tracks_.clear();  // drops every retained track + queue + normalizer
  mid_by_mline_.clear();
  state_.store(ConnectionState::kClosed);
}

void WebrtcReceiver::setRemoteDescription(const std::string& type, const std::string& sdp) {
  if (!pc_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(tracks_mutex_);
    mid_by_mline_ = extractMidsInOrder(sdp);
  }
  try {
    pc_->setRemoteDescription(rtc::Description(sdp, type));
    primeFromRemoteSdp(sdp);
  } catch (const std::exception& e) {
    // libdatachannel rejected the offer synchronously: no answer, no ICE, no
    // state change. Surface it so the owner does not stall silently forever.
    if (on_error_) {
      on_error_(std::string("setRemoteDescription failed: ") + e.what());
    }
  }
}

void WebrtcReceiver::addRemoteCandidate(const std::string& candidate, int mline_index) {
  if (!pc_) {
    return;
  }
  std::string mid;
  {
    std::lock_guard<std::mutex> lk(tracks_mutex_);
    if (mline_index >= 0 && static_cast<size_t>(mline_index) < mid_by_mline_.size()) {
      mid = mid_by_mline_[static_cast<size_t>(mline_index)];
    }
  }
  if (mid.empty()) {
    mid = std::to_string(mline_index);  // fallback (no a=mid)
  }
  try {
    pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
  } catch (const std::exception&) {}
}

void WebrtcReceiver::primeFromRemoteSdp(const std::string& sdp) {
  const auto sprop_by_mid = extractSpropPerMid(sdp);
  std::lock_guard<std::mutex> lk(tracks_mutex_);
  for (const auto& [mid, sprop] : sprop_by_mid) {
    auto it = tracks_.find(mid);
    if (it == tracks_.end()) {
      continue;
    }
    primeNormalizerFromSprop(it->second.normalizer, sprop);
  }
}

void WebrtcReceiver::onFrame(const std::string& mid, const uint8_t* data, size_t size, uint32_t /*rtp_ts*/) {
  if (data == nullptr || size == 0) {
    return;
  }
  // normalize() allocates and copies the whole access unit; keep it OFF the
  // lock. normalize() is const (only prime() mutates), so under the lock we grab
  // a stable pointer to the normalizer, release, normalize into a local frame,
  // then re-lock only to enqueue. std::map nodes are stable across inserts, but
  // a concurrent close()/open() can clear the map, so re-find before pushing.
  const H264AnnexBNormalizer* normalizer = nullptr;
  {
    std::lock_guard<std::mutex> lk(tracks_mutex_);
    auto it = tracks_.find(mid);
    if (it == tracks_.end()) {
      return;
    }
    normalizer = &it->second.normalizer;
  }

  bool keyframe = false;
  std::vector<uint8_t> annexb = normalizer->normalize(data, size, keyframe);
  if (annexb.empty()) {
    return;
  }
  EncodedFrame ef;
  ef.ts_ns = wallClockNs();
  ef.keyframe = keyframe;
  ef.annexb = std::move(annexb);

  std::lock_guard<std::mutex> lk(tracks_mutex_);
  auto it = tracks_.find(mid);
  if (it == tracks_.end()) {
    return;  // track removed (close/reopen) while normalizing: drop the frame
  }
  it->second.queue.push(std::move(ef));
}

std::vector<std::pair<std::string, EncodedFrame>> WebrtcReceiver::drainByStream() {
  std::vector<std::pair<std::string, EncodedFrame>> out;
  std::lock_guard<std::mutex> lk(tracks_mutex_);
  for (auto& [mid, ts] : tracks_) {
    while (!ts.queue.empty()) {
      out.emplace_back(ts.stream_id, std::move(ts.queue.front()));
      ts.queue.pop();
    }
  }
  return out;
}

std::vector<std::string> WebrtcReceiver::extractMidsInOrderForTest(const std::string& sdp) {
  return extractMidsInOrder(sdp);
}

std::map<std::string, std::string> WebrtcReceiver::extractSpropPerMidForTest(const std::string& sdp) {
  return extractSpropPerMid(sdp);
}

bool WebrtcReceiver::acceptTrackForTest(const std::string& mid) {
  // Mirrors the accept/drop decision in onTrack (minus retaining a real
  // rtc::Track): a known mid is accepted; an unknown mid is dropped unless the
  // subscribe set was empty (manual/legacy accept-any-track), in which case it
  // is admitted with the fallback frame_id.
  std::lock_guard<std::mutex> lk(tracks_mutex_);
  auto it = tracks_.find(mid);
  if (it == tracks_.end()) {
    if (!expected_empty_) {
      return false;  // a mid we never subscribed to: drop it
    }
    TrackState ts;
    ts.stream_id = mid;
    ts.frame_id = config_.frame_id;
    tracks_.emplace(mid, std::move(ts));
  }
  return true;
}

EncodedFrame WebrtcReceiver::normalizeAccessUnit(
    const uint8_t* au, size_t size, int64_t ts_ns, const std::string& sprop_parameter_sets) {
  H264AnnexBNormalizer normalizer;
  primeNormalizerFromSprop(normalizer, sprop_parameter_sets);
  EncodedFrame ef;
  ef.ts_ns = ts_ns;
  ef.annexb = normalizer.normalize(au, size, ef.keyframe);
  return ef;
}

}  // namespace webrtc
}  // namespace PJ
