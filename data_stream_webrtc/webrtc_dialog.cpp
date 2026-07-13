// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "webrtc_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <utility>

#include "datastream_webrtc_ui.hpp"
#include "webrtc_manifest.hpp"

namespace webrtc_dialog_detail {

namespace {
constexpr std::chrono::seconds kAutoFetchPeriod{1};
}  // namespace

WebrtcDialog::~WebrtcDialog() {
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();
  }
}

std::string WebrtcDialog::manifest() const {
  return kWebrtcManifest;
}

std::string WebrtcDialog::ui_content() const {
  return kDataStreamWebrtcUi;
}

std::string WebrtcDialog::widget_data() {
  PJ::WidgetData wd;

  wd.setText("lineEditServerUrl", server_url_);
  wd.setText("lineEditBearer", bearer_);
  wd.setText("lineEditApiUrl", api_url_);
  wd.setText("lineEditTopicPrefix", topic_prefix_);
  wd.setText("lineEditManualPath", manual_path_);

  wd.setTableHeaders("camerasList", {"Path", "Tracks", "Ready"});
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> selected_labels;
  {
    std::lock_guard<std::mutex> lock(catalog_mutex_);
    wd.setText("labelApiStatus", api_status_);
    rows.reserve(catalog_.size() + manual_paths_.size());
    for (const auto& p : catalog_) {
      if (!passesFilter(p.name)) {
        continue;
      }
      if (isSelected(p.name)) {
        selected_labels.push_back(p.name);
      }
      rows.push_back({p.name, joinTracks(p.tracks), p.ready ? "yes" : "no"});
    }
    for (const auto& m : manual_paths_) {
      const bool in_catalog =
          std::any_of(catalog_.begin(), catalog_.end(), [&m](const auto& p) { return p.name == m; });
      if (in_catalog || !passesFilter(m)) {
        continue;
      }
      if (isSelected(m)) {
        selected_labels.push_back(m);
      }
      rows.push_back({m, "(manual)", "-"});
    }
  }
  wd.setTableRows("camerasList", rows);
  if (!selected_labels.empty()) {
    // Selection restored by first-column TEXT (setSelectedItems): stays
    // correct however the sortable table is currently ordered.
    wd.setSelectedItems("camerasList", selected_labels);
  }

  wd.setTableHeaders("tableIceServers", {"URL", "Username", "Credential"});
  std::vector<std::vector<std::string>> ice_rows;
  for (const auto& srv : ice_servers_) {
    ice_rows.push_back({srv.url, srv.username, srv.credential});
  }
  wd.setTableRows("tableIceServers", ice_rows);

  wd.setOkEnabled(!selected_.empty());
  return wd.toJson();
}

bool WebrtcDialog::onClicked(std::string_view widget_name) {
  if (widget_name == "buttonRefresh") {
    startFetch();
    return true;
  }
  if (widget_name == "buttonAddPath") {
    if (!manual_path_.empty()) {
      if (std::find(manual_paths_.begin(), manual_paths_.end(), manual_path_) == manual_paths_.end()) {
        manual_paths_.push_back(manual_path_);
      }
      if (!isSelected(manual_path_)) {
        selected_.push_back(manual_path_);
      }
      manual_path_.clear();
    }
    return true;
  }
  return false;
}

bool WebrtcDialog::onTextChanged(std::string_view widget_name, std::string_view text) {
  if (widget_name == "lineEditServerUrl") {
    server_url_ = std::string(text);
    if (!api_url_edited_) {
      api_url_ = deriveApiUrl(server_url_);
      return true;
    }
    return false;
  }
  if (widget_name == "lineEditBearer") {
    bearer_ = std::string(text);
    return false;
  }
  if (widget_name == "lineEditApiUrl") {
    api_url_ = std::string(text);
    api_url_edited_ = true;
    return false;
  }
  if (widget_name == "lineEditTopicPrefix") {
    topic_prefix_ = std::string(text);
    return false;
  }
  if (widget_name == "lineEditManualPath") {
    manual_path_ = std::string(text);
    return false;
  }
  if (widget_name == "lineEditFilter") {
    filter_ = std::string(text);
    filter_lower_ = toLower(filter_);
    return true;
  }
  return false;
}

bool WebrtcDialog::onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) {
  if (widget_name != "camerasList") {
    return false;
  }
  std::lock_guard<std::mutex> lock(catalog_mutex_);
  // The host reports the selection of the currently RENDERED rows only. A
  // selected path drops out of the selection ONLY when the user deselects a row
  // that is currently rendered; a selection that is merely not rendered right
  // now (filter-hidden, OR gone from the catalog on a transient fetch failure)
  // must survive. So: preserve every selected path that is not a rendered row,
  // then re-add the rendered ones the host still reports (subject to the
  // H.264/manual selectable check).
  std::vector<std::string> next;
  for (const auto& sel : selected_) {
    if (!isRenderedLocked(sel)) {
      next.push_back(sel);  // not a visible row: the host can't report it, keep it
    }
  }
  for (const auto& label : selected) {
    if (std::find(next.begin(), next.end(), label) != next.end()) {
      continue;
    }
    const bool manual = std::find(manual_paths_.begin(), manual_paths_.end(), label) != manual_paths_.end();
    bool selectable = manual;
    if (!selectable) {
      for (const auto& p : catalog_) {
        if (p.name == label) {
          selectable = hasH264(p.tracks);
          break;
        }
      }
    }
    if (selectable) {
      next.push_back(label);
    }
  }
  selected_ = std::move(next);
  return true;
}

bool WebrtcDialog::onTick() {
  bool dirty = false;
  if (fetch_done_.load()) {
    harvestFetch();
    dirty = true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!fetch_in_flight_ && !api_url_.empty() && (now - last_fetch_) > kAutoFetchPeriod) {
    startFetch();
  }
  return dirty;
}

void WebrtcDialog::onAccepted(std::string_view /*json*/) {}

void WebrtcDialog::onRejected() {}

std::string WebrtcDialog::saveConfig() const {
  nlohmann::json cfg;
  cfg["server_url"] = server_url_;
  cfg["bearer_token"] = bearer_;
  cfg["api_url"] = api_url_;
  // Persist the derive-vs-manual intent explicitly. saveConfig ALWAYS writes
  // api_url, so keying "edited" off its presence would wrongly latch manual
  // after any round-trip and stop server_url edits from re-deriving the host.
  cfg["api_url_edited"] = api_url_edited_;
  cfg["topic_prefix"] = topic_prefix_;
  cfg["selected"] = selected_;
  cfg["manual_paths"] = manual_paths_;
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
  // Pre-WHEP configs (address/port/our_id/manual_stream) have no meaningful
  // mapping and are ignored: unknown keys fall through to these defaults.
  server_url_ = cfg.value("server_url", std::string("http://127.0.0.1:8889"));
  bearer_ = cfg.value("bearer_token", std::string());
  api_url_ = cfg.value("api_url", deriveApiUrl(server_url_));
  // Restore the explicit intent, NOT api_url presence: a never-edited api_url
  // reloads as still-auto-derived so editing server_url re-derives the host.
  api_url_edited_ = cfg.value("api_url_edited", false);
  topic_prefix_ = cfg.value("topic_prefix", std::string("webrtc"));

  selected_.clear();
  if (cfg.contains("selected") && cfg["selected"].is_array()) {
    for (const auto& e : cfg["selected"]) {
      if (e.is_string() && !e.get<std::string>().empty()) {
        selected_.push_back(e.get<std::string>());
      }
    }
  }
  manual_paths_.clear();
  if (cfg.contains("manual_paths") && cfg["manual_paths"].is_array()) {
    for (const auto& e : cfg["manual_paths"]) {
      if (e.is_string() && !e.get<std::string>().empty()) {
        manual_paths_.push_back(e.get<std::string>());
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

bool WebrtcDialog::hasH264(const std::vector<std::string>& tracks) {
  for (const auto& t : tracks) {
    if (t.find("264") != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string WebrtcDialog::joinTracks(const std::vector<std::string>& tracks) {
  std::string out;
  for (const auto& t : tracks) {
    if (!out.empty()) {
      out += ", ";
    }
    out += t;
  }
  return out.empty() ? "-" : out;
}

std::string WebrtcDialog::deriveApiUrl(const std::string& server_url) {
  const size_t scheme_end = server_url.find("://");
  if (scheme_end == std::string::npos) {
    return "http://127.0.0.1:9997";
  }
  const size_t host_start = scheme_end + 3;
  // Bracket-aware: for an IPv6 literal ("http://[::1]:8889") the host runs
  // through the matching ']', and the port (if any) is a ':' AFTER that ']';
  // splitting on the first ':' would slice the address itself.
  size_t host_end;
  if (host_start < server_url.size() && server_url[host_start] == '[') {
    const size_t bracket_end = server_url.find(']', host_start);
    if (bracket_end == std::string::npos) {
      host_end = server_url.size();  // malformed; keep the whole authority
    } else {
      host_end = bracket_end + 1;  // include ']' in the host, drop any ":port" after
    }
  } else {
    host_end = server_url.find(':', host_start);
    if (host_end == std::string::npos) {
      host_end = server_url.find('/', host_start);
    }
    if (host_end == std::string::npos) {
      host_end = server_url.size();
    }
  }
  return server_url.substr(0, scheme_end) + "://" + server_url.substr(host_start, host_end - host_start) + ":9997";
}

bool WebrtcDialog::passesFilter(const std::string& path) const {
  if (filter_lower_.empty()) {
    return true;
  }
  return toLower(path).find(filter_lower_) != std::string::npos;
}

bool WebrtcDialog::isSelected(const std::string& path) const {
  return std::find(selected_.begin(), selected_.end(), path) != selected_.end();
}

bool WebrtcDialog::isRenderedLocked(const std::string& path) const {
  // Mirror widget_data()'s row set exactly: a catalog entry passing the filter,
  // or a manual path (not shadowed by the catalog) passing the filter.
  const bool in_catalog =
      std::any_of(catalog_.begin(), catalog_.end(), [&path](const auto& p) { return p.name == path; });
  if (in_catalog) {
    return passesFilter(path);
  }
  const bool is_manual = std::find(manual_paths_.begin(), manual_paths_.end(), path) != manual_paths_.end();
  return is_manual && passesFilter(path);
}

void WebrtcDialog::startFetch() {
  if (fetch_in_flight_) {
    return;
  }
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();  // a finished-but-unharvested worker
  }
  fetch_in_flight_ = true;
  fetch_done_.store(false);
  last_fetch_ = std::chrono::steady_clock::now();
  const std::string url = api_url_;
  const std::string token = bearer_;
  fetch_thread_ = std::thread([this, url, token]() {
    auto out = PJ::webrtc::fetchPathsList(url, token, std::chrono::seconds(2));
    {
      std::lock_guard<std::mutex> lock(catalog_mutex_);
      if (out) {
        fetch_result_ = std::move(*out);
        fetch_status_ = std::to_string(fetch_result_.size()) + " path(s) on the server";
      } else {
        fetch_result_.clear();
        fetch_status_ = "Control API: " + out.error().message + " (manual paths still work)";
      }
    }
    fetch_done_.store(true);
  });
}

void WebrtcDialog::harvestFetch() {
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();
  }
  fetch_in_flight_ = false;
  fetch_done_.store(false);
  std::lock_guard<std::mutex> lock(catalog_mutex_);
  catalog_ = std::move(fetch_result_);
  api_status_ = std::move(fetch_status_);
}

}  // namespace webrtc_dialog_detail
