// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// WebRTC multi-camera streaming client DataSource. Thin wrapper over the Qt-free
// cores (video_emit / webrtc_receiver / webrtc_signaling). One PeerConnection
// carries N recvonly H.264 tracks; each selected camera becomes one canonical
// PJ.VideoFrame topic. The dialog discovers a catalog and the source steals its
// live signaling socket so it does not re-handshake.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "video_emit.hpp"
#include "webrtc_dialog.hpp"
#include "webrtc_manifest.hpp"
#include "webrtc_receiver.hpp"
#include "webrtc_signaling.hpp"

namespace {

// Per-camera runtime record. Keyed by mid (== stream id).
struct StreamBinding {
  std::string mid;
  std::string topic;
  std::string frame_id;
  PJ::ParserBindingHandle binding{};
};

class WebrtcSource : public PJ::StreamSourceBase {
 public:
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
    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = static_cast<uint16_t>(cfg.value("port", 8443));
    our_id_ = cfg.value("our_id", std::string("receiver"));
    topic_prefix_ = cfg.value("topic_prefix", std::string("webrtc"));
    manual_stream_ = cfg.value("manual_stream", std::string());

    selected_.clear();
    if (cfg.contains("selected") && cfg["selected"].is_array()) {
      for (const auto& e : cfg["selected"]) {
        SelStream s;
        s.id = e.value("id", std::string());
        s.name = e.value("name", std::string());
        if (!s.id.empty()) {
          selected_.push_back(std::move(s));
        }
      }
    }
    if (selected_.empty() && !manual_stream_.empty()) {
      selected_.push_back({manual_stream_, manual_stream_});  // N=1 manual stream
    }
    // (selected_.empty() && manual_stream_.empty()) => legacy wait-for-offer:
    // subscribe to nothing; bind lazily from the runtime mid in onPoll.

    webrtc_config_ = PJ::webrtc::WebrtcConfig{};
    webrtc_config_.frame_id = topic_prefix_.empty() ? "webrtc" : topic_prefix_;
    if (cfg.contains("ice_servers") && cfg["ice_servers"].is_array()) {
      for (const auto& e : cfg["ice_servers"]) {
        PJ::webrtc::IceServerConfig srv;
        srv.url = e.value("url", std::string());
        srv.username = e.value("username", std::string());
        srv.credential = e.value("credential", std::string());
        if (!srv.url.empty()) {
          webrtc_config_.ice_servers.push_back(std::move(srv));
        }
      }
    }

    stolen_signaling_ = dialog_.takeSignaling();

    backoff_ms_ = kMinBackoffMs;
    reconnect_at_poll_ = 0;
    poll_count_ = 0;
    connect();
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    ++poll_count_;

    if (receiver_) {
      auto frames = receiver_->drainByStream();  // vector<pair<stream_id, EncodedFrame>>
      for (auto& [stream_id, ef] : frames) {
        StreamBinding* sb = bindingForMid(stream_id);  // lazily binds unknown mids
        if (sb == nullptr) {
          continue;
        }
        auto status = PJ::webrtc::pushVideoFrame(runtimeHost(), sb->binding, ef, sb->frame_id);
        if (!status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning, "video frame push failed (" + stream_id + "): " + status.error());
        }
      }
    }

    if (connection_lost_.exchange(false)) {
      teardownConnection();
      reconnect_at_poll_ = poll_count_ + backoffPolls();
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, "WebRTC peer disconnected; reconnecting...");
    }
    if (receiver_ == nullptr && signaling_ == nullptr && reconnect_at_poll_ != 0 && poll_count_ >= reconnect_at_poll_) {
      reconnect_at_poll_ = 0;
      backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
      connect();
    }
    return PJ::okStatus();
  }

  void onStop() override {
    teardownConnection();
    streams_.clear();
    reconnect_at_poll_ = 0;
  }

 private:
  struct SelStream {
    std::string id;
    std::string name;
  };

  static constexpr int kMinBackoffMs = 500;
  static constexpr int kMaxBackoffMs = 8000;
  static constexpr int kPollPeriodMs = 33;

  uint64_t backoffPolls() const {
    return static_cast<uint64_t>(std::max(1, backoff_ms_ / kPollPeriodMs));
  }

  static std::string makeTopic(const std::string& prefix, const std::string& leaf) {
    std::string p = prefix;
    while (!p.empty() && p.back() == '/') {
      p.pop_back();
    }
    return p.empty() ? leaf : (p + "/" + leaf);
  }

  StreamBinding* bindingForMid(const std::string& mid) {
    auto it = streams_.find(mid);
    if (it != streams_.end()) {
      return &it->second;
    }
    std::string leaf = mid;
    for (const auto& s : selected_) {
      if (s.id == mid && !s.name.empty()) {
        leaf = s.name;
        break;
      }
    }
    StreamBinding sb;
    sb.mid = mid;
    sb.frame_id = leaf;
    sb.topic = makeTopic(topic_prefix_, leaf);
    auto binding = runtimeHost().ensureParserBinding({
        .topic_name = sb.topic,
        .parser_encoding = "protobuf",
        .type_name = "PJ.VideoFrame",
        .schema = {},
        .parser_config_json = {},
    });
    if (!binding) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "failed to bind PJ.VideoFrame for stream '" + mid + "': " + binding.error());
      return nullptr;
    }
    sb.binding = *binding;
    auto [ins, _] = streams_.emplace(mid, std::move(sb));
    return &ins->second;
  }

  // Build the StreamSpec list the receiver pre-creates tracks from.
  std::vector<PJ::webrtc::StreamSpec> streamSpecs() const {
    std::vector<PJ::webrtc::StreamSpec> specs;
    specs.reserve(selected_.size());
    for (const auto& s : selected_) {
      const std::string leaf = s.name.empty() ? s.id : s.name;
      specs.push_back({s.id, leaf});  // frame_id == leaf
    }
    return specs;  // empty => receiver accepts any single track (manual/legacy)
  }

  std::vector<std::string> selectedMids() const {
    std::vector<std::string> mids;
    mids.reserve(selected_.size());
    for (const auto& s : selected_) {
      mids.push_back(s.id);
    }
    return mids;
  }

  void connect() {
    receiver_ = std::make_unique<PJ::webrtc::WebrtcReceiver>();
    if (stolen_signaling_) {
      signaling_ = std::move(stolen_signaling_);
    } else {
      signaling_ = std::make_unique<PJ::webrtc::WebrtcSignaling>();
    }

    receiver_->setLocalDescriptionCallback(
        [this](const std::string& type, const std::string& sdp) { signaling_->sendSdp(type, sdp); });
    receiver_->setLocalCandidateCallback(
        [this](const std::string& cand, int mline) { signaling_->sendIce(cand, mline); });
    receiver_->setStateCallback([this](PJ::webrtc::ConnectionState s) {
      if (s == PJ::webrtc::ConnectionState::kDisconnected || s == PJ::webrtc::ConnectionState::kFailed ||
          s == PJ::webrtc::ConnectionState::kClosed) {
        connection_lost_.store(true);
      } else if (s == PJ::webrtc::ConnectionState::kConnected) {
        backoff_ms_ = kMinBackoffMs;
      }
    });

    signaling_->setSdpCallback(
        [this](const std::string& type, const std::string& sdp) { receiver_->setRemoteDescription(type, sdp); });
    signaling_->setIceCallback(
        [this](const std::string& cand, int mline) { receiver_->addRemoteCandidate(cand, mline); });
    signaling_->setClosedCallback([this]() { connection_lost_.store(true); });

    // Subscribe must go out only once the WS is open. The dialog's stolen socket
    // is already open (send inline below); a fresh socket fires this on connect.
    pending_subscribe_ = selectedMids();
    signaling_->setConnectedCallback([this]() {
      if (!pending_subscribe_.empty() && signaling_) {
        signaling_->subscribe(pending_subscribe_);
        pending_subscribe_.clear();
      }
    });

    auto status = receiver_->open(webrtc_config_, streamSpecs());
    if (!status) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "WebRTC receiver open failed: " + status.error());
      teardownConnection();
      reconnect_at_poll_ = poll_count_ + backoffPolls();
      return;
    }

    if (!signaling_->isOpen()) {
      PJ::webrtc::SignalingConfig sig;
      sig.url = "ws://" + address_ + ":" + std::to_string(port_);
      sig.our_id = our_id_.empty() ? "receiver" : our_id_;
      sig.peer_id = "";
      signaling_->open(sig);
    } else if (!pending_subscribe_.empty()) {
      // Reused (stolen) socket: already past onOpen, so the connected callback
      // won't refire. Send the subscribe inline.
      signaling_->subscribe(pending_subscribe_);
      pending_subscribe_.clear();
    }
  }

  void teardownConnection() {
    // Tear the receiver down FIRST. Its libdatachannel callbacks capture `this`
    // and call into signaling_ (onLocalCandidate -> sendIce, onLocalDescription
    // -> sendSdp) on a worker thread. If signaling_ were reset first, an in-flight
    // candidate/description callback could deref the now-null signaling_ unique_ptr.
    // The dependency is one-directional (receiver -> signaling), so stop it first.
    if (receiver_) {
      receiver_->close();
      receiver_.reset();
    }
    if (signaling_) {
      signaling_->close();
      signaling_.reset();
    }
    connection_lost_.store(false);
    pending_subscribe_.clear();
    // Keep streams_ (bindings) across a reconnect — same topics resume.
  }

  WebrtcDialog dialog_;

  std::string address_ = "127.0.0.1";
  uint16_t port_ = 8443;
  std::string our_id_ = "receiver";
  std::string topic_prefix_ = "webrtc";
  std::string manual_stream_;
  std::vector<SelStream> selected_;

  PJ::webrtc::WebrtcConfig webrtc_config_;
  std::map<std::string, StreamBinding> streams_;
  std::vector<std::string> pending_subscribe_;

  std::unique_ptr<PJ::webrtc::WebrtcSignaling> stolen_signaling_;
  std::unique_ptr<PJ::webrtc::WebrtcReceiver> receiver_;
  std::unique_ptr<PJ::webrtc::WebrtcSignaling> signaling_;

  std::atomic<bool> connection_lost_{false};
  int backoff_ms_ = kMinBackoffMs;
  uint64_t poll_count_ = 0;
  uint64_t reconnect_at_poll_ = 0;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(WebrtcSource, kWebrtcManifest)
PJ_DIALOG_PLUGIN(WebrtcDialog)
