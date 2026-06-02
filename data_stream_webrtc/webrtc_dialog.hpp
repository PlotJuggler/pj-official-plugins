// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Smart discovery dialog for the multi-camera WebRTC client. Owns the signaling
// socket during the dialog session for catalog discovery; on accept the source
// steals it via takeSignaling() so the SDP handshake is not redone. The plugin
// model is the single source of truth and round-trips via saveConfig/loadConfig.
#pragma once

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "datastream_webrtc_ui.hpp"
#include "webrtc_manifest.hpp"
#include "webrtc_signaling.hpp"

namespace {

struct IceRow {
  std::string url;
  std::string username;
  std::string credential;
};

// A camera the user picked, captured so the source can subscribe even after the
// live catalog is gone (saved-layout reload).
struct SelectedStream {
  std::string id;    // == mid contract
  std::string name;  // topic leaf
  std::string codec = "h264";
  int width = 0;
  int height = 0;
};

class WebrtcDialog : public PJ::DialogPluginTyped {
 public:
  ~WebrtcDialog() override {
    disconnect();
  }

  // Transfer the live signaling connection to the source (do NOT close it).
  std::unique_ptr<PJ::webrtc::WebrtcSignaling> takeSignaling() {
    connected_ = false;
    return std::move(signaling_);
  }

  std::string manifest() const override {
    return kWebrtcManifest;
  }
  std::string ui_content() const override {
    return kDataStreamWebrtcUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setText("lineEditOurId", our_id_);
    wd.setEnabled("lineEditAddress", !connected_);
    wd.setEnabled("lineEditPort", !connected_);
    wd.setButtonText("buttonConnect", connected_ ? "Connected" : "Connect");
    wd.setChecked("buttonConnect", connected_.load());

    wd.setText("lineEditTopicPrefix", topic_prefix_);
    wd.setText("lineEditManualStream", manual_stream_);

    wd.setTableHeaders("camerasList", {"Camera", "Codec", "Resolution"});
    std::vector<std::vector<std::string>> rows;
    std::vector<int> selected_rows;
    {
      std::lock_guard<std::mutex> lock(catalog_mutex_);
      rows.reserve(catalog_.size());
      const std::string flt = toLower(filter_);
      for (const auto& s : catalog_) {
        const std::string label = s.name.empty() ? s.id : s.name;
        const std::string codec = s.codec.empty() ? "h264" : s.codec;
        if (!flt.empty() && toLower(label).find(flt) == std::string::npos &&
            toLower(codec).find(flt) == std::string::npos) {
          continue;
        }
        const std::string res =
            (s.width > 0 && s.height > 0) ? (std::to_string(s.width) + " x " + std::to_string(s.height)) : "-";
        // Mark this just-pushed row as selected if its stream id is in selected_.
        // camerasList is a QTableWidget: the host restores selection from
        // selected_rows (view.selectedRows()/selectRow), not selected_items, so
        // we must hand it row indices computed against THIS filtered row set.
        for (const auto& sel : selected_) {
          if (sel.id == s.id) {
            selected_rows.push_back(static_cast<int>(rows.size()));
            break;
          }
        }
        rows.push_back({label, codec, res});
      }
    }
    wd.setTableRows("camerasList", rows);

    if (!selected_rows.empty()) {
      wd.setSelectedRows("camerasList", selected_rows);
    }

    wd.setTableHeaders("tableIceServers", {"URL", "Username", "Credential"});
    std::vector<std::vector<std::string>> ice_rows;
    for (const auto& srv : ice_servers_) {
      ice_rows.push_back({srv.url, srv.username, srv.credential});
    }
    wd.setTableRows("tableIceServers", ice_rows);

    // OK: connected AND (>=1 camera selected OR a manual id typed OR the
    // legacy empty-catalog single-offer path).
    const bool has_target = !selected_.empty() || !manual_stream_.empty() || (connected_ && catalogEmpty());
    wd.setOkEnabled(connected_ && has_target);
    return wd.toJson();
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonConnect") {
      if (!connected_) {
        connect();
      } else {
        disconnect();
      }
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditAddress") {
      address_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPort") {
      const int v = std::atoi(std::string(text).c_str());
      if (v > 0 && v <= 65535) {
        port_ = v;
      }
      return false;
    }
    if (widget_name == "lineEditOurId") {
      our_id_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditTopicPrefix") {
      topic_prefix_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditManualStream") {
      manual_stream_ = std::string(text);
      return true;  // toggles OK-enabled when no cameras selected
    }
    if (widget_name == "lineEditFilter") {
      filter_ = std::string(text);
      return true;  // re-filter
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "camerasList") {
      std::lock_guard<std::mutex> lock(catalog_mutex_);
      selected_.clear();
      for (const auto& label : selected) {
        for (const auto& s : catalog_) {
          const std::string s_label = s.name.empty() ? s.id : s.name;
          if (s_label == label) {
            selected_.push_back({s.id, s.name, s.codec, s.width, s.height});
            break;
          }
        }
      }
      return true;
    }
    return false;
  }

  bool onTick() override {
    if (catalog_dirty_.exchange(false)) {
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {
    // Do NOT disconnect — the source's onStart() steals the signaling socket.
  }
  void onRejected() override {
    disconnect();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    cfg["our_id"] = our_id_;
    cfg["topic_prefix"] = topic_prefix_;
    cfg["manual_stream"] = manual_stream_;

    nlohmann::json sel = nlohmann::json::array();
    for (const auto& s : selected_) {
      sel.push_back(
          {{"id", s.id},
           {"name", s.name},
           {"mid", s.id},
           {"codec", s.codec},
           {"width", s.width},
           {"height", s.height}});
    }
    cfg["selected"] = sel;

    nlohmann::json ice = nlohmann::json::array();
    for (const auto& srv : ice_servers_) {
      ice.push_back({{"url", srv.url}, {"username", srv.username}, {"credential", srv.credential}});
    }
    cfg["ice_servers"] = ice;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = cfg.value("port", 8443);
    our_id_ = cfg.value("our_id", std::string("receiver"));
    topic_prefix_ = cfg.value("topic_prefix", std::string("webrtc"));
    manual_stream_ = cfg.value("manual_stream", std::string());

    selected_.clear();
    if (cfg.contains("selected") && cfg["selected"].is_array()) {
      for (const auto& e : cfg["selected"]) {
        SelectedStream s;
        s.id = e.value("id", std::string());
        s.name = e.value("name", std::string());
        s.codec = e.value("codec", std::string("h264"));
        s.width = e.value("width", 0);
        s.height = e.value("height", 0);
        if (!s.id.empty()) {
          selected_.push_back(std::move(s));
        }
      }
    }
    ice_servers_.clear();
    if (cfg.contains("ice_servers") && cfg["ice_servers"].is_array()) {
      for (const auto& e : cfg["ice_servers"]) {
        IceRow r;
        r.url = e.value("url", std::string());
        r.username = e.value("username", std::string());
        r.credential = e.value("credential", std::string());
        if (!r.url.empty()) {
          ice_servers_.push_back(std::move(r));
        }
      }
    }
    return true;
  }

 private:
  static std::string toLower(std::string s) {
    for (auto& c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }
  bool catalogEmpty() const {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    return catalog_.empty();
  }

  void connect() {
    signaling_ = std::make_unique<PJ::webrtc::WebrtcSignaling>();
    signaling_->setCatalogCallback([this](std::vector<PJ::webrtc::DiscoveredStream> streams) {
      {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        catalog_ = std::move(streams);
      }
      catalog_dirty_.store(true);
    });
    signaling_->setConnectedCallback([this]() {
      connected_ = true;
      catalog_dirty_.store(true);
      if (signaling_) {
        signaling_->requestList();  // ask for the catalog (broker may also push)
      }
    });
    signaling_->setClosedCallback([this]() {
      connected_ = false;
      {
        std::lock_guard<std::mutex> lock(catalog_mutex_);
        catalog_.clear();
      }
      catalog_dirty_.store(true);
    });

    PJ::webrtc::SignalingConfig sig;
    sig.url = "ws://" + address_ + ":" + std::to_string(port_);
    sig.our_id = our_id_.empty() ? "receiver" : our_id_;
    sig.peer_id = "";  // discovery: register and wait for the catalog
    signaling_->open(sig);
  }

  void disconnect() {
    if (signaling_) {
      signaling_->close();
      signaling_.reset();
    }
    connected_ = false;
  }

  std::string address_ = "127.0.0.1";
  int port_ = 8443;
  std::string our_id_ = "receiver";
  std::string topic_prefix_ = "webrtc";
  std::string manual_stream_;
  std::string filter_;

  std::atomic<bool> connected_ = false;
  std::atomic<bool> catalog_dirty_ = false;
  std::unique_ptr<PJ::webrtc::WebrtcSignaling> signaling_;

  mutable std::mutex catalog_mutex_;
  std::vector<PJ::webrtc::DiscoveredStream> catalog_;
  std::vector<SelectedStream> selected_;
  std::vector<IceRow> ice_servers_;
};

}  // namespace
