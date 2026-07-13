// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "whep_connection.hpp"

#include <chrono>
#include <rtc/rtc.hpp>
#include <utility>

namespace PJ {
namespace webrtc {
namespace {

// First "sprop-parameter-sets=<b64,b64>" value in the given text. Empty if
// absent. (The answer has a single video m-line, so first match wins.)
std::string extractFirstSprop(const std::string& sdp) {
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

WhepConnection::WhepConnection() = default;

WhepConnection::~WhepConnection() {
  close();
}

int64_t WhepConnection::wallClockNs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

void WhepConnection::reportState(ConnectionState s) {
  state_.store(s);
  if (on_state_) {
    on_state_(s);
  }
}

PJ::Status WhepConnection::open(const WhepConnectionConfig& config) {
  if (pc_ || worker_.joinable()) {
    return PJ::unexpected("already open — call close() first");
  }
  config_ = config;
  if (config_.whep_url.empty()) {
    return PJ::unexpected("empty WHEP URL");
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

  pc_->onStateChange([this](rtc::PeerConnection::State s) { reportState(toConnectionState(s)); });

  pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
    if (s == rtc::PeerConnection::GatheringState::Complete) {
      {
        std::lock_guard<std::mutex> lk(gather_mutex_);
        gathering_done_ = true;
      }
      gather_cv_.notify_all();
    }
  });

  // OFFERER (WHEP contract): add the single recvonly H.264 track ourselves;
  // media arrives on it after the answer is applied. onTrack never fires.
  try {
    rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
    media.addH264Codec(config_.h264_payload_type);
    track_ = pc_->addTrack(media);  // RETAIN

    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
    depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track_->setMediaHandler(depacketizer);
    track_->onFrame([this](rtc::binary frame, rtc::FrameInfo /*info*/) {
      onFrame(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
    });

    pc_->setLocalDescription();  // builds the offer, starts ICE gathering
  } catch (const std::exception& e) {
    if (track_) {
      try {
        track_->resetCallbacks();
      } catch (...) {}
    }
    if (pc_) {
      try {
        pc_->resetCallbacks();
      } catch (...) {}
    }
    pc_.reset();
    track_.reset();
    return PJ::unexpected(std::string("offer setup failed: ") + e.what());
  }

  reportState(ConnectionState::kConnecting);
  worker_ = std::thread([this]() { runConnect(); });
  return PJ::okStatus();
}

void WhepConnection::close() {
  {
    std::lock_guard<std::mutex> lk(gather_mutex_);
    closing_ = true;
  }
  gather_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::string session;
  {
    std::lock_guard<std::mutex> lk(session_mutex_);
    session.swap(session_url_);
  }
  if (!session.empty()) {
    deleteSession(session, config_.bearer_token, config_.delete_timeout);
  }
  if (track_) {
    try {
      track_->resetCallbacks();
    } catch (...) {}
  }
  if (pc_) {
    try {
      pc_->resetCallbacks();
      pc_->close();
    } catch (...) {}
    pc_.reset();
  }
  track_.reset();
  on_state_ = {};
  on_error_ = {};
  {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    std::queue<EncodedFrame> empty;
    queue_.swap(empty);
  }
  {
    std::lock_guard<std::mutex> lk(gather_mutex_);
    closing_ = false;  // allow reopen
    gathering_done_ = false;
  }
  state_.store(ConnectionState::kClosed);
}

void WhepConnection::onFrame(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  // normalize() allocates and copies; it is const on the normalizer (only
  // prime mutates, once, before media can flow), but a concurrent close()
  // clears state — so both the normalize and the enqueue take frames_mutex_.
  bool keyframe = false;
  std::vector<uint8_t> annexb;
  {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    annexb = normalizer_.normalize(data, size, keyframe);
  }
  if (annexb.empty()) {
    return;
  }
  EncodedFrame ef;
  ef.ts_ns = wallClockNs();
  ef.keyframe = keyframe;
  ef.annexb = std::move(annexb);
  std::lock_guard<std::mutex> lk(frames_mutex_);
  queue_.push(std::move(ef));
}

std::vector<EncodedFrame> WhepConnection::drain() {
  std::vector<EncodedFrame> out;
  std::lock_guard<std::mutex> lk(frames_mutex_);
  while (!queue_.empty()) {
    out.push_back(std::move(queue_.front()));
    queue_.pop();
  }
  return out;
}

void WhepConnection::runConnect() {
  {
    std::unique_lock<std::mutex> lk(gather_mutex_);
    const bool done =
        gather_cv_.wait_for(lk, config_.connect_timeout, [this]() { return gathering_done_ || closing_; });
    if (closing_) {
      return;
    }
    if (!done || !gathering_done_) {
      if (on_error_) {
        on_error_(WhepErrorKind::kTimeout, "ICE gathering timed out");
      }
      return;
    }
  }

  std::string offer_sdp;
  try {
    auto local = pc_->localDescription();
    if (!local) {
      if (on_error_) {
        on_error_(WhepErrorKind::kBadResponse, "no local description after gathering");
      }
      return;
    }
    offer_sdp = std::string(*local);
  } catch (const std::exception& e) {
    if (on_error_) {
      on_error_(WhepErrorKind::kBadResponse, std::string("local description: ") + e.what());
    }
    return;
  }

  auto out = postOffer(config_.whep_url, config_.bearer_token, offer_sdp, config_.connect_timeout);
  bool was_closing = false;
  {
    std::lock_guard<std::mutex> lk(gather_mutex_);
    was_closing = closing_;
  }
  if (was_closing) {
    // close() raced the POST: if it succeeded, hang up the orphan session
    // (outside every mutex — blocking HTTP must not stall gather_mutex_
    // waiters, incl. libdatachannel pool threads in onGatheringStateChange).
    if (out) {
      deleteSession(out->session_url, config_.bearer_token, config_.delete_timeout);
    }
    return;
  }
  if (!out) {
    if (on_error_) {
      on_error_(out.error().kind, "WHEP POST failed: " + out.error().message);
    }
    return;
  }
  // No closing_ re-check needed here: if close() set closing_ after the check
  // above, it is now blocked in worker_.join() until this function returns,
  // and only THEN swaps session_url_ out — so this store always
  // happens-before close()'s DELETE. No session leaks.
  {
    std::lock_guard<std::mutex> lk(session_mutex_);
    session_url_ = out->session_url;
  }

  try {
    pc_->setRemoteDescription(rtc::Description(out->answer_sdp, "answer"));
  } catch (const std::exception& e) {
    if (on_error_) {
      on_error_(WhepErrorKind::kBadResponse, std::string("answer rejected: ") + e.what());
    }
    return;
  }

  // Prime SPS/PPS from the answer: the decoder needs them ahead of the first
  // keyframe; some publishers only announce them in SDP. In-band parameter
  // sets still pass through the normalizer untouched.
  const std::string sprop = extractFirstSprop(out->answer_sdp);
  if (!sprop.empty()) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    primeNormalizerFromSprop(normalizer_, sprop);
  }
  // Connected/Failed now arrives via pc_->onStateChange.
}

EncodedFrame WhepConnection::normalizeAccessUnit(
    const uint8_t* au, size_t size, int64_t ts_ns, const std::string& sprop_parameter_sets) {
  H264AnnexBNormalizer normalizer;
  primeNormalizerFromSprop(normalizer, sprop_parameter_sets);
  EncodedFrame ef;
  ef.ts_ns = ts_ns;
  ef.annexb = normalizer.normalize(au, size, ef.keyframe);
  return ef;
}

std::string WhepConnection::extractSpropForTest(const std::string& answer_sdp) {
  return extractFirstSprop(answer_sdp);
}

void WhepConnection::primeFromAnswerForTest(const std::string& answer_sdp) {
  const std::string sprop = extractFirstSprop(answer_sdp);
  if (!sprop.empty()) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    primeNormalizerFromSprop(normalizer_, sprop);
  }
}

}  // namespace webrtc
}  // namespace PJ
