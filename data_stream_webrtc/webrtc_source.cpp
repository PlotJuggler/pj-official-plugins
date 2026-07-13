// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// WHEP multi-camera streaming client DataSource. Thin wrapper over the Qt-free
// cores (video_emit / whep_connection / whep_client). Each selected mediamtx
// path gets its OWN WhepConnection (one PC, one recvonly H.264 track) and one
// canonical PJ.VideoFrame topic <prefix>/<path>. Per-camera reconnect with
// exponential backoff; 401/403 is terminal for that camera until restart.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "video_emit.hpp"
#include "webrtc_dialog.hpp"
#include "webrtc_manifest.hpp"
#include "whep_client.hpp"
#include "whep_connection.hpp"

namespace {

class WebrtcSource : public PJ::StreamSourceBase {
 public:
  // The host may destroy the source without a preceding onStop() (the SDK
  // data_source_handle destroys without calling stop()). Tearing down here
  // guarantees every WhepConnection is closed (workers joined, callbacks
  // detached) while the atomics + message queue those callbacks capture are
  // still alive — otherwise their destruction would race the teardown.
  // Idempotent: onStop() clears cameras_, so a normal Stop leaves the map
  // empty and this loop is a no-op.
  ~WebrtcSource() override {
    onStop();
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }
  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }
  PJ::Status loadConfig(std::string_view config_json) override {
    if (!config_json.empty()) {
      (void)dialog_.loadConfig(config_json);
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    auto cfg = nlohmann::json::parse(dialog_.saveConfig(), nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected("invalid dialog config");
    }
    server_url_ = cfg.value("server_url", std::string("http://127.0.0.1:8889"));
    bearer_token_ = cfg.value("bearer_token", std::string());
    topic_prefix_ = cfg.value("topic_prefix", std::string("webrtc"));

    ice_servers_.clear();
    if (cfg.contains("ice_servers") && cfg["ice_servers"].is_array()) {
      for (const auto& e : cfg["ice_servers"]) {
        PJ::webrtc::IceServerConfig srv;
        srv.url = e.value("url", std::string());
        srv.username = e.value("username", std::string());
        srv.credential = e.value("credential", std::string());
        if (!srv.url.empty()) {
          ice_servers_.push_back(std::move(srv));
        }
      }
    }

    std::vector<std::string> paths;
    if (cfg.contains("selected") && cfg["selected"].is_array()) {
      for (const auto& e : cfg["selected"]) {
        if (e.is_string() && !e.get<std::string>().empty()) {
          paths.push_back(e.get<std::string>());
        }
      }
    }
    if (paths.empty()) {
      return PJ::unexpected("no stream paths selected");
    }

    // A restart must let the current selection win: drop any cameras left from a
    // previous Start (teardownCamera is safe on a not-yet-connected camera) so
    // stale paths don't linger and only the selected ones are rebuilt below.
    for (auto& [path, cam] : cameras_) {
      teardownCamera(*cam);
    }
    cameras_.clear();

    poll_count_ = 0;
    for (const auto& path : paths) {
      if (cameras_.count(path) == 0) {
        auto cam = std::make_unique<CameraRuntime>();
        cam->path = path;
        cam->topic = makeTopic(topic_prefix_, path);
        cam->frame_id = path;
        cameras_.emplace(path, std::move(cam));
      }
    }
    for (auto& [path, cam] : cameras_) {
      connectCamera(*cam);
    }
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    ++poll_count_;
    drainMessages();

    for (auto& [path, cam] : cameras_) {
      if (cam->connected.exchange(false)) {
        cam->backoff_ms = kMinBackoffMs;
        cam->failed_attempts = 0;
        cam->escalated = false;
      }

      if (cam->conn) {
        for (auto& ef : cam->conn->drain()) {
          if (!ensureBinding(*cam)) {
            break;
          }
          auto status = PJ::webrtc::pushVideoFrame(runtimeHost(), cam->binding, ef, cam->frame_id);
          if (!status) {
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kWarning, "video frame push failed (" + cam->path + "): " + status.error());
          }
        }
      }

      if (cam->lost.exchange(false)) {
        teardownCamera(*cam);
        if (cam->terminal.load()) {
          cam->reconnect_at_poll = 0;
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kError,
              "WHEP '" + cam->path + "' stopped: authorization rejected. Fix the Bearer token and Start again.");
        } else {
          cam->reconnect_at_poll = poll_count_ + backoffPolls(cam->backoff_ms);
          ++cam->failed_attempts;
          if (cam->failed_attempts >= kEscalateAfterAttempts && !cam->escalated) {
            cam->escalated = true;
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kError, "WHEP '" + cam->path + "': still failing after " +
                                                        std::to_string(cam->failed_attempts) +
                                                        " attempts (check server URL / that the path is published).");
          } else {
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kInfo, "WHEP '" + cam->path + "' disconnected; reconnecting...");
          }
        }
      }

      if (!cam->conn && cam->reconnect_at_poll != 0 && poll_count_ >= cam->reconnect_at_poll) {
        cam->reconnect_at_poll = 0;
        cam->backoff_ms = std::min(cam->backoff_ms * 2, kMaxBackoffMs);
        connectCamera(*cam);
      }
    }
    return PJ::okStatus();
  }

  void onStop() override {
    for (auto& [path, cam] : cameras_) {
      teardownCamera(*cam);
      cam->reconnect_at_poll = 0;
      cam->terminal.store(false);
      cam->connected.store(false);
      cam->backoff_ms = kMinBackoffMs;
      cam->failed_attempts = 0;
      cam->escalated = false;
    }
    cameras_.clear();
  }

 private:
  static constexpr int kMinBackoffMs = 500;
  static constexpr int kMaxBackoffMs = 8000;
  static constexpr int kPollPeriodMs = 33;

  // Per-camera runtime. Held by unique_ptr so the atomics stay put; the
  // connection's callbacks capture the raw pointer, which stays valid because
  // teardownCamera() closes the connection (joining its worker and detaching
  // callbacks) before the CameraRuntime is ever destroyed.
  struct CameraRuntime {
    std::string path;
    std::string topic;
    std::string frame_id;
    PJ::ParserBindingHandle binding{};
    bool bound = false;
    std::unique_ptr<PJ::webrtc::WhepConnection> conn;
    int backoff_ms = kMinBackoffMs;
    uint64_t reconnect_at_poll = 0;
    // Consecutive non-terminal connect failures; drives a one-shot kError
    // escalation so a persistent bad server_url is diagnosable without spam.
    int failed_attempts = 0;
    bool escalated = false;
    std::atomic<bool> lost{false};
    std::atomic<bool> terminal{false};
    // Set by the state callback on kConnected (worker thread); the poll thread
    // consumes it to clear the failure counters without racing them.
    std::atomic<bool> connected{false};
  };

  // After this many consecutive non-terminal failures, report ONCE at kError.
  static constexpr int kEscalateAfterAttempts = 5;

  static uint64_t backoffPolls(int backoff_ms) {
    return static_cast<uint64_t>(std::max(1, backoff_ms / kPollPeriodMs));
  }

  static std::string makeTopic(const std::string& prefix, const std::string& leaf) {
    std::string p = prefix;
    while (!p.empty() && p.back() == '/') {
      p.pop_back();
    }
    return p.empty() ? leaf : (p + "/" + leaf);
  }

  // Queue a diagnostic from a worker thread; drained in onPoll (the cores never
  // touch the host directly).
  void queueMessage(std::string msg) {
    std::lock_guard<std::mutex> lk(messages_mutex_);
    pending_messages_.push_back(std::move(msg));
  }

  // Report any worker-thread diagnostics on the poll thread.
  void drainMessages() {
    std::vector<std::string> msgs;
    {
      std::lock_guard<std::mutex> lk(messages_mutex_);
      msgs.swap(pending_messages_);
    }
    for (const auto& m : msgs) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, m);
    }
  }

  bool ensureBinding(CameraRuntime& cam) {
    if (cam.bound) {
      return true;
    }
    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = cam.topic,
        .parser_encoding = "protobuf",
        .type_name = "PJ.VideoFrame",
        .schema = {},
        .parser_config_json = {},
    });
    if (!binding) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "failed to bind PJ.VideoFrame for '" + cam.path + "': " + binding.error());
      return false;
    }
    cam.binding = *binding;
    cam.bound = true;
    return true;
  }

  void connectCamera(CameraRuntime& cam) {
    cam.conn = std::make_unique<PJ::webrtc::WhepConnection>();
    CameraRuntime* rt = &cam;
    // Set BOTH callbacks BEFORE open(). They fire on internal/worker threads;
    // they only flip atomics + queue diagnostics — never close()/reset the
    // connection (that happens on the poll thread via teardownCamera()).
    cam.conn->setStateCallback([rt](PJ::webrtc::ConnectionState s) {
      if (s == PJ::webrtc::ConnectionState::kDisconnected || s == PJ::webrtc::ConnectionState::kFailed ||
          s == PJ::webrtc::ConnectionState::kClosed) {
        rt->lost.store(true);
      } else if (s == PJ::webrtc::ConnectionState::kConnected) {
        rt->connected.store(true);  // poll thread clears backoff + failure counters
      }
    });
    cam.conn->setErrorCallback([this, rt](PJ::webrtc::WhepErrorKind kind, const std::string& reason) {
      queueMessage("WHEP '" + rt->path + "': " + reason);
      // 401/403 is terminal for this camera until the user fixes the token.
      // Latch terminal BEFORE lost so onPoll (which reads terminal only after
      // lost fires the teardown) observes it; the two are independent atomics,
      // and a connect-phase failure also sets state kFailed (which sets lost
      // via the state callback) — order-independent either way.
      if (kind == PJ::webrtc::WhepErrorKind::kUnauthorized) {
        rt->terminal.store(true);
      }
      rt->lost.store(true);
    });

    PJ::webrtc::WhepConnectionConfig cfg;
    cfg.whep_url = PJ::webrtc::buildWhepUrl(server_url_, cam.path);
    cfg.bearer_token = bearer_token_;
    cfg.ice_servers = ice_servers_;
    if (auto st = cam.conn->open(cfg); !st) {
      queueMessage("WHEP open failed ('" + cam.path + "'): " + st.error());
      cam.conn.reset();
      cam.reconnect_at_poll = poll_count_ + backoffPolls(cam.backoff_ms);
    }
  }

  void teardownCamera(CameraRuntime& cam) {
    if (cam.conn) {
      cam.conn->close();  // joins the worker + detaches callbacks: rt* safe after
      cam.conn.reset();
    }
    cam.lost.store(false);
    // Keep the binding across reconnects — same topic resumes.
  }

  webrtc_dialog_detail::WebrtcDialog dialog_;

  std::string server_url_ = "http://127.0.0.1:8889";
  std::string bearer_token_;
  std::string topic_prefix_ = "webrtc";
  std::vector<PJ::webrtc::IceServerConfig> ice_servers_;

  std::map<std::string, std::unique_ptr<CameraRuntime>> cameras_;

  std::mutex messages_mutex_;
  std::vector<std::string> pending_messages_;
  uint64_t poll_count_ = 0;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(WebrtcSource, kWebrtcManifest)
PJ_DIALOG_PLUGIN(webrtc_dialog_detail::WebrtcDialog)
