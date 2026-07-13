// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Headless WHEP receiver probe — exercises the REAL plugin cores
// (WhepConnection + whep_client + the H264 Annex-B normalizer) without
// PlotJuggler: connects to a WHEP endpoint (e.g. mediamtx), receives one
// H.264 stream, and writes the normalized Annex-B to a file while printing
// live frame/keyframe/byte counts.
//
//   ./webrtc_recv_probe http://127.0.0.1:8889/cam0/whep [--token T] [--out F]
//   verify: ffplay /tmp/webrtc_recv.h264
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <pj_base/sdk/platform.hpp>
#include <rtc/rtc.hpp>
#include <string>
#include <thread>

#include "whep_connection.hpp"

namespace {
std::atomic<bool> g_stop{false};
void onSigint(int /*sig*/) {
  g_stop.store(true);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <whep-url> [--token T] [--out F]\n", argv[0]);
    return 1;
  }
  const std::string whep_url = argv[1];
  std::string token;
  std::string out_path = "/tmp/webrtc_recv.h264";
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string val = argv[i + 1];
    if (key == "--token") {
      token = val;
    } else if (key == "--out") {
      out_path = val;
    }
  }
  std::signal(SIGINT, onSigint);
  // libdatachannel diagnostics: Warning by default (quiet — the [recv] counter
  // is the probe's main output). Set RTC_LOG=info|debug to trace ICE/DTLS.
  const auto rtc_log = PJ::sdk::getEnv("RTC_LOG");
  rtc::InitLogger(
      rtc_log == "debug"  ? rtc::LogLevel::Debug
      : rtc_log == "info" ? rtc::LogLevel::Info
                          : rtc::LogLevel::Warning);

  std::ofstream out(out_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "cannot open output file: %s\n", out_path.c_str());
    return 1;
  }

  PJ::webrtc::WhepConnection conn;
  conn.setStateCallback(
      [](PJ::webrtc::ConnectionState s) { std::fprintf(stderr, "[state] %d\n", static_cast<int>(s)); });
  conn.setErrorCallback([](PJ::webrtc::WhepErrorKind k, const std::string& reason) {
    std::fprintf(stderr, "[error] kind=%d %s\n", static_cast<int>(k), reason.c_str());
    g_stop.store(true);
  });

  PJ::webrtc::WhepConnectionConfig cfg;
  cfg.whep_url = whep_url;
  cfg.bearer_token = token;
  if (auto st = conn.open(cfg); !st) {
    std::fprintf(stderr, "open failed: %s\n", st.error().c_str());
    return 1;
  }
  std::fprintf(stderr, "WHEP %s (Ctrl-C to stop). Writing %s\n", whep_url.c_str(), out_path.c_str());

  std::uint64_t frames = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t bytes = 0;
  auto last_print = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    for (auto& ef : conn.drain()) {
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
  conn.close();
  out.close();
  std::fprintf(
      stderr, "\nDone: %llu frames (%llu keyframes), %llu bytes -> %s\n", static_cast<unsigned long long>(frames),
      static_cast<unsigned long long>(keyframes), static_cast<unsigned long long>(bytes), out_path.c_str());
  return frames > 0 ? 0 : 2;
}
