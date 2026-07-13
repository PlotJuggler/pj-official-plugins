// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free, libdatachannel-free WHEP HTTP client (IETF WISH draft, the protocol
// mediamtx / go2rtc / LiveKit serve): POST an SDP offer to
// <server>/<path>/whep, get the answer + a session URL (Location header),
// DELETE the session URL to hang up. Also wraps the mediamtx Control API
// (GET /v3/paths/list) for stream discovery. Blocking calls — run them on a
// worker thread, never on the poll thread or a libdatachannel callback.
#pragma once

#include <chrono>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

namespace PJ {
namespace webrtc {

enum class WhepErrorKind {
  kUnauthorized,  // HTTP 401/403 — terminal until the user fixes the token
  kNotFound,      // HTTP 404 — path exists but no publisher yet: retry
  kTimeout,       // connect/transfer timeout: retry
  kNetwork,       // DNS/TCP/read failure: retry
  kBadResponse,   // unexpected status / missing Location / empty body
};

// Typed error payload for PJ::Expected so callers can classify retry-vs-terminal.
struct WhepError {
  WhepErrorKind kind = WhepErrorKind::kNetwork;
  std::string message;
};

struct WhepResult {
  std::string answer_sdp;
  std::string session_url;  // absolute URL for DELETE
};

// One entry of the mediamtx Control API /v3/paths/list response.
struct WhepPathInfo {
  std::string name;
  bool ready = false;
  std::vector<std::string> tracks;  // codec names, e.g. "H264"
};

// "<server_url>/<path>/whep" with exactly one '/' at each joint.
std::string buildWhepUrl(const std::string& server_url, const std::string& path);

// Resolve a Location header against the request URL: absolute (has "://")
// passed through; "/abs/path" keeps scheme://host[:port]; "rel/path" replaces
// the last request-URL segment.
std::string resolveLocation(const std::string& request_url, const std::string& location);

[[nodiscard]] PJ::Expected<WhepResult, WhepError> postOffer(
    const std::string& whep_url, const std::string& bearer_token, const std::string& offer_sdp,
    std::chrono::seconds timeout);

// Best-effort session teardown; errors are ignored (the server reaps stale
// sessions on ICE timeout anyway).
void deleteSession(const std::string& session_url, const std::string& bearer_token, std::chrono::seconds timeout);

// GET <api_url>/v3/paths/list and parse items[].{name,ready,tracks}.
[[nodiscard]] PJ::Expected<std::vector<WhepPathInfo>, WhepError> fetchPathsList(
    const std::string& api_url, const std::string& bearer_token, std::chrono::seconds timeout);

}  // namespace webrtc
}  // namespace PJ
