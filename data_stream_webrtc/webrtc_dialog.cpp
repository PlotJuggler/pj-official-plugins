// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "webrtc_dialog.hpp"

#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <utility>
#include <vector>

#include "datastream_webrtc_ui.hpp"
#include "webrtc_manifest.hpp"

namespace webrtc_dialog_detail {

WebrtcDialog::~WebrtcDialog() {
  disconnect();
}

std::unique_ptr<PJ::webrtc::WebrtcSignaling> WebrtcDialog::takeSignaling() {
  connected_ = false;
  return std::move(signaling_);
}

std::string WebrtcDialog::manifest() const {
  return kWebrtcManifest;
}

std::string WebrtcDialog::ui_content() const {
  return kDataStreamWebrtcUi;
}

std::string WebrtcDialog::widget_data() {
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
    const auto base_counts = buildBaseNameCounts(catalog_);
    for (const auto& s : catalog_) {
      const std::string label = displayLabel(s, base_counts);
      const std::string codec = s.codec.empty() ? "h264" : s.codec;
      if (!passesFilter(label, codec)) {
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

bool WebrtcDialog::onClicked(std::string_view widget_name) {
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

bool WebrtcDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
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
    return true;
  }
  if (widget_name == "lineEditFilter") {
    filter_ = std::string(text);
    filter_lower_ = toLower(filter_);
    return true;
  }
  return false;
}

bool WebrtcDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name == "camerasList") {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    const auto base_counts = buildBaseNameCounts(catalog_);
    // The host reports the selection of the CURRENTLY rendered (filtered) table
    // only, so a previously-picked camera that the active filter hides is
    // absent here. Rebuilding from scratch would silently unselect it. Keep
    // those still-selected-but-now-hidden entries (matched by id) and rebuild
    // the visible portion from the reported labels.
    std::vector<SelectedStream> preserved;
    for (const auto& sel : selected_) {
      bool visible = false;
      for (const auto& s : catalog_) {
        if (s.id == sel.id && passesFilter(displayLabel(s, base_counts), s.codec)) {
          visible = true;
          break;
        }
      }
      if (!visible) {
        preserved.push_back(sel);
      }
    }
    selected_ = std::move(preserved);
    // Resolve each reported label back to its id via the same unique label the
    // rows were rendered with (1:1 even when names collide).
    for (const auto& label : selected) {
      for (const auto& s : catalog_) {
        if (displayLabel(s, base_counts) == label) {
          selected_.push_back({s.id, s.name});
          break;
        }
      }
    }
    return true;
  }
  return false;
}

bool WebrtcDialog::onTick() {
  if (catalog_dirty_.exchange(false)) {
    return true;
  }
  return false;
}

void WebrtcDialog::onAccepted(std::string_view /*json*/) {
  // Do NOT disconnect — the source's onStart() steals the signaling socket.
}

void WebrtcDialog::onRejected() {
  disconnect();
}

std::string WebrtcDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["address"] = address_;
  cfg["port"] = port_;
  cfg["our_id"] = our_id_;
  cfg["topic_prefix"] = topic_prefix_;
  cfg["manual_stream"] = manual_stream_;

  nlohmann::json sel = nlohmann::json::array();
  for (const auto& s : selected_) {
    sel.push_back({{"id", s.id}, {"name", s.name}, {"mid", s.id}});
  }
  cfg["selected"] = sel;

  nlohmann::json ice = nlohmann::json::array();
  for (const auto& srv : ice_servers_) {
    ice.push_back({{"url", srv.url}, {"username", srv.username}, {"credential", srv.credential}});
  }
  cfg["ice_servers"] = ice;
  return cfg.dump();
}

bool WebrtcDialog::loadConfig(std::string_view config_json) {
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

std::string WebrtcDialog::toLower(std::string s) {
  for (auto& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string WebrtcDialog::baseLabel(const PJ::webrtc::DiscoveredStream& s) {
  return s.name.empty() ? s.id : s.name;
}

std::unordered_map<std::string, int> WebrtcDialog::buildBaseNameCounts(
    const std::vector<PJ::webrtc::DiscoveredStream>& catalog) {
  std::unordered_map<std::string, int> counts;
  counts.reserve(catalog.size());
  for (const auto& s : catalog) {
    ++counts[baseLabel(s)];
  }
  return counts;
}

std::string WebrtcDialog::displayLabel(
    const PJ::webrtc::DiscoveredStream& s, const std::unordered_map<std::string, int>& counts) {
  const std::string base = baseLabel(s);
  const auto it = counts.find(base);
  const bool duplicate = it != counts.end() && it->second > 1;
  return duplicate ? (base + " (" + s.id + ")") : base;
}

bool WebrtcDialog::passesFilter(const std::string& label, const std::string& codec_in) const {
  if (filter_lower_.empty()) {
    return true;
  }
  const std::string codec = codec_in.empty() ? "h264" : codec_in;
  return toLower(label).find(filter_lower_) != std::string::npos ||
         toLower(codec).find(filter_lower_) != std::string::npos;
}

bool WebrtcDialog::catalogEmpty() const {
  std::lock_guard<std::mutex> lock(catalog_mutex_);
  return catalog_.empty();
}

void WebrtcDialog::reconcileSelectedWithCatalogLocked() {
  for (auto& sel : selected_) {
    for (const auto& s : catalog_) {
      if (s.id == sel.id) {
        sel.name = s.name;
        break;
      }
    }
  }
}

void WebrtcDialog::connect() {
  signaling_ = std::make_unique<PJ::webrtc::WebrtcSignaling>();
  signaling_->setCatalogCallback([this](std::vector<PJ::webrtc::DiscoveredStream> streams) {
    {
      std::lock_guard<std::mutex> lock(catalog_mutex_);
      catalog_ = std::move(streams);
      reconcileSelectedWithCatalogLocked();
    }
    catalog_dirty_.store(true);
  });
  signaling_->setConnectedCallback([this]() {
    connected_ = true;
    catalog_dirty_.store(true);
    if (signaling_) {
      // Ask for the catalog (the broker may also push it).
      signaling_->requestList();
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
  // Discovery: register and wait for the catalog.
  sig.peer_id = "";
  signaling_->open(sig);
}

void WebrtcDialog::disconnect() {
  if (signaling_) {
    signaling_->close();
    signaling_.reset();
  }
  connected_ = false;
}

}  // namespace webrtc_dialog_detail
