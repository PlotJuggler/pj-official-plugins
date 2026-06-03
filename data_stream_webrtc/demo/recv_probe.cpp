// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Headless WebRTC receiver probe — the answerer half of the demo/ rig, without
// PlotJuggler. Exercises the REAL plugin cores (WebrtcReceiver + WebrtcSignaling
// + the H264 Annex-B normalizer): connects to stream_server.py, receives an
// H.264 stream from the GStreamer sender, and writes the normalized Annex-B to a
// file while printing live frame/keyframe/byte counts. Confirms the receive path
// end-to-end (camera -> WebRTC -> plugin) before building the full app.
//
//   ./webrtc_recv_probe --port 8443 --our receiver --out /tmp/webrtc_recv.h264
//   then (sender): python3 send_camera.py --server ws://127.0.0.1:8443
//                      --device /dev/video0 --peer receiver
//   verify:        ffplay /tmp/webrtc_recv.h264
//                  (or pipe it through: gst-launch-1.0 filesrc
//                   location=/tmp/webrtc_recv.h264 ! h264parse ! avdec_h264 !
//                   videoconvert ! autovideosink)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <pj_base/sdk/platform.hpp>
#include <rtc/rtc.hpp>
#include <string>
#include <thread>

#include "webrtc_receiver.hpp"
#include "webrtc_signaling.hpp"

namespace {
std::atomic<bool> g_stop{false};
void onSigint(int /*sig*/) {
  g_stop.store(true);
}
}  // namespace

int main(int argc, char** argv) {
  std::string address = "127.0.0.1";
  int port = 8443;
  std::string our_id = "receiver";
  std::string peer_id;
  std::string out_path = "/tmp/webrtc_recv.h264";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string val = argv[i + 1];
    if (key == "--addr") {
      address = val;
    } else if (key == "--port") {
      port = std::stoi(val);
    } else if (key == "--our") {
      our_id = val;
    } else if (key == "--peer") {
      peer_id = val;
    } else if (key == "--out") {
      out_path = val;
    }
  }
  std::signal(SIGINT, onSigint);
  // libdatachannel diagnostics: Warning by default (quiet — the [recv] counter
  // is the probe's main output). Set RTC_LOG=info|debug to trace ICE/DTLS.
  const auto rtc_log = PJ::sdk::getEnv("RTC_LOG");  // SDK helper: portable, MSVC-clean
  rtc::InitLogger(
      rtc_log == "debug"  ? rtc::LogLevel::Debug
      : rtc_log == "info" ? rtc::LogLevel::Info
                          : rtc::LogLevel::Warning);

  PJ::webrtc::WebrtcReceiver receiver;
  PJ::webrtc::WebrtcSignaling signaling;

  // Wire the two cores together exactly as WebrtcSource does.
  receiver.setLocalDescriptionCallback(
      [&signaling](const std::string& type, const std::string& sdp) { signaling.sendSdp(type, sdp); });
  receiver.setLocalCandidateCallback(
      [&signaling](const std::string& cand, int mline) { signaling.sendIce(cand, mline); });
  receiver.setStateCallback(
      [](PJ::webrtc::ConnectionState s) { std::fprintf(stderr, "[state] %d\n", static_cast<int>(s)); });
  signaling.setSdpCallback(
      [&receiver](const std::string& type, const std::string& sdp) { receiver.setRemoteDescription(type, sdp); });
  signaling.setIceCallback(
      [&receiver](const std::string& cand, int mline) { receiver.addRemoteCandidate(cand, mline); });
  signaling.setClosedCallback([]() { std::fprintf(stderr, "[signaling] closed\n"); });

  PJ::webrtc::WebrtcConfig wc;
  wc.frame_id = "probe";
  if (auto st = receiver.open(wc, {}); !st) {  // empty expected => accept any track
    std::fprintf(stderr, "receiver.open failed: %s\n", st.error().c_str());
    return 1;
  }
  PJ::webrtc::SignalingConfig sc;
  sc.url = "ws://" + address + ":" + std::to_string(port);
  sc.our_id = our_id;
  sc.peer_id = peer_id;
  signaling.open(sc);

  std::fprintf(
      stderr, "Listening as '%s' on %s (Ctrl-C to stop). Writing %s\n", our_id.c_str(), sc.url.c_str(),
      out_path.c_str());

  std::ofstream out(out_path, std::ios::binary);
  std::uint64_t frames = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t bytes = 0;
  auto last_print = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    auto batch = receiver.drainByStream();
    for (auto& [stream_id, ef] : batch) {
      (void)stream_id;  // single .h264 file: all cameras' frames interleave
      out.write(reinterpret_cast<const char*>(ef.annexb.data()), static_cast<std::streamsize>(ef.annexb.size()));
      bytes += ef.annexb.size();
      ++frames;
      if (ef.keyframe) {
        ++keyframes;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_print > std::chrono::milliseconds(500)) {
      std::fprintf(
          stderr, "\r[recv] frames=%llu keyframes=%llu bytes=%llu   ", static_cast<unsigned long long>(frames),
          static_cast<unsigned long long>(keyframes), static_cast<unsigned long long>(bytes));
      std::fflush(stderr);
      last_print = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  out.close();
  std::fprintf(
      stderr, "\nDone: %llu frames (%llu keyframes), %llu bytes -> %s\n", static_cast<unsigned long long>(frames),
      static_cast<unsigned long long>(keyframes), static_cast<unsigned long long>(bytes), out_path.c_str());
  return frames > 0 ? 0 : 2;
}
