// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Discovery dialog for the WHEP multi-camera client. Polls the mediamtx
// Control API (/v3/paths/list) on a worker thread while open (~1 Hz + manual
// Refresh); manual path entry covers servers without the API. Selection is
// restored by TEXT (path name) so the sortable table stays consistent. The
// plugin model is the single source of truth via saveConfig/loadConfig.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "whep_client.hpp"

namespace webrtc_dialog_detail {

struct IceRow {
  std::string url;
  std::string username;
  std::string credential;
};

class WebrtcDialog : public PJ::DialogPluginTyped {
 public:
  ~WebrtcDialog() override;

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
  static bool hasH264(const std::vector<std::string>& tracks);
  static std::string joinTracks(const std::vector<std::string>& tracks);
  // "http://host:8889" -> "http://host:9997" (mediamtx Control API default).
  static std::string deriveApiUrl(const std::string& server_url);

  // Bridge server_url_ / api_url_ to/from their split Transport/Address/Port fields.
  // Each stays the canonical value saveConfig persists; parse splits it for display,
  // compose rebuilds it on edit. Transport is fixed http:// (HTTPS is not wired in
  // this build), so compose always emits an http:// URL. Both are IPv6-literal aware
  // ("http://[::1]:8889"), mirroring deriveApiUrl's authority parsing.
  struct UrlParts {
    std::string host;
    std::string port;
    std::string path;
  };
  static UrlParts parseHttpUrl(const std::string& url);
  static std::string composeHttpUrl(const std::string& host, const std::string& port, const std::string& path);

  // Whitespace-trimmed copy of a user-typed path.
  static std::string trimmedPath(std::string s);
  // True if the path still carries whitespace/control bytes that could only
  // fail later as an opaque URL error — reject at input time instead.
  static bool hasSpaceOrControl(const std::string& s);

  bool passesFilter(const std::string& path) const;
  bool isSelected(const std::string& path) const;
  // True iff `path` is currently a visible row (catalog entry passing the
  // filter, or a manual path not in the catalog passing the filter) — i.e. a
  // row the host could report in onSelectionChanged. Caller holds catalog_mutex_.
  bool isRenderedLocked(const std::string& path) const;
  void startFetch();    // spawn the worker if none in flight
  void harvestFetch();  // join a finished worker, publish results

  std::string server_url_ = "http://127.0.0.1:8889";
  std::string bearer_;
  std::string api_url_ = "http://127.0.0.1:9997";
  bool api_url_edited_ = false;  // stop auto-deriving once the user typed one
  std::string topic_prefix_ = "webrtc";
  std::string manual_path_;
  std::string filter_;
  std::string filter_lower_;

  // Catalog fetched from the Control API + fetch machinery. The worker thread
  // writes fetch_* under catalog_mutex_ and sets fetch_done_; onTick harvests.
  mutable std::mutex catalog_mutex_;
  std::vector<PJ::webrtc::WhepPathInfo> catalog_;
  std::string api_status_;
  std::vector<PJ::webrtc::WhepPathInfo> fetch_result_;
  std::string fetch_status_;
  std::thread fetch_thread_;
  // Abort handle for the fetch in flight (fresh per startFetch): flipping it
  // on reject/destroy bounds the fetch_thread_ join to milliseconds instead
  // of the remaining pages' HTTP timeouts.
  PJ::webrtc::HttpAbortPtr fetch_abort_;
  std::atomic<bool> fetch_done_{false};
  bool fetch_in_flight_ = false;
  std::chrono::steady_clock::time_point last_fetch_{};

  std::vector<std::string> selected_;      // path names (text-keyed)
  std::vector<std::string> manual_paths_;  // user-added rows, persisted
  std::vector<IceRow> ice_servers_;
};

}  // namespace webrtc_dialog_detail
