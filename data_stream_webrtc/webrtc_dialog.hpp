// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Smart discovery dialog for the multi-camera WebRTC client. Owns the signaling
// socket during the dialog session for catalog discovery; on accept the source
// steals it via takeSignaling() so the SDP handshake is not redone. The plugin
// model is the single source of truth and round-trips via saveConfig/loadConfig.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "webrtc_signaling.hpp"

namespace webrtc_dialog_detail {

struct IceRow {
  std::string url;
  std::string username;
  std::string credential;
};

// A camera the user picked, captured so the source can subscribe even after the
// live catalog is gone (saved-layout reload). Only id (== mid contract) and name
// (topic leaf) are needed downstream; codec/resolution come from the live
// catalog at render time, so they are not persisted here.
struct SelectedStream {
  std::string id;    // == mid contract
  std::string name;  // topic leaf
};

class WebrtcDialog : public PJ::DialogPluginTyped {
 public:
  ~WebrtcDialog() override;

  // Transfer the live signaling connection to the source (do NOT close it).
  std::unique_ptr<PJ::webrtc::WebrtcSignaling> takeSignaling();

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;

  bool onClicked(std::string_view widget_name) override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onTick() override;

  void onAccepted(std::string_view json) override;
  void onRejected() override;

  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;

 private:
  static std::string toLower(std::string s);
  static std::string baseLabel(const PJ::webrtc::DiscoveredStream& s);
  static std::unordered_map<std::string, int> buildBaseNameCounts(
      const std::vector<PJ::webrtc::DiscoveredStream>& catalog);
  static std::string displayLabel(
      const PJ::webrtc::DiscoveredStream& s, const std::unordered_map<std::string, int>& counts);

  bool passesFilter(const std::string& label, const std::string& codec_in) const;
  bool catalogEmpty() const;
  void reconcileSelectedWithCatalogLocked();
  void connect();
  void disconnect();

  std::string address_ = "127.0.0.1";
  int port_ = 8443;
  std::string our_id_ = "receiver";
  std::string topic_prefix_ = "webrtc";
  std::string manual_stream_;
  std::string filter_;
  std::string filter_lower_;  // cached toLower(filter_); kept in sync in onTextChanged

  std::atomic<bool> connected_ = false;
  std::atomic<bool> catalog_dirty_ = false;
  std::unique_ptr<PJ::webrtc::WebrtcSignaling> signaling_;

  mutable std::mutex catalog_mutex_;
  std::vector<PJ::webrtc::DiscoveredStream> catalog_;
  std::vector<SelectedStream> selected_;
  std::vector<IceRow> ice_servers_;
};

}  // namespace webrtc_dialog_detail
