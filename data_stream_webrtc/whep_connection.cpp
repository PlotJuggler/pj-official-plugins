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
  config_ = config;
  return PJ::unexpected("not implemented");  // Task 6
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
  // Implemented in Task 6.
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
