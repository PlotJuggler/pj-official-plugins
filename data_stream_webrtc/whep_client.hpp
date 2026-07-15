// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free, libdatachannel-free WHEP HTTP client (IETF WISH draft, the protocol
// mediamtx / go2rtc / LiveKit serve): POST an SDP offer to
// <server>/<path>/whep, get the answer + a session URL (Location header),
// DELETE the session URL to hang up. Also wraps the mediamtx Control API
// (GET /v3/paths/list) for stream discovery. Blocking calls — run them on a
// worker thread, never on the poll thread or a libdatachannel callback.
//
// Deliberate WHEP-scope limitations (documented, not bugs): no trickle-ICE
// PATCH / 406 counter-offer flow, no redirect following, no Retry-After
// parsing (callers just retry with backoff), RFC3986-lite Location
// resolution, and Bearer-over-http is allowed (LAN use; prefer https where
// tokens matter).
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

namespace PJ {
namespace webrtc {

// Cumulative response-byte accounting across ixwebsocket progress callbacks.
// Socket::readBytes reports (bytes read so far in THIS call, this call's read
// length), and chunked transfers run one readBytes per chunk — so `current`
// alone under-counts a chunked response (each chunk restarts from 0). A new
// call is detected when `current` drops OR `total` changes; the previous
// call's bytes are then banked. Equal-size chunks each delivered in a single
// recv show neither signal and slip past this detector —
// postOffer/fetchPathsList keep their post-hoc body-size check as the second
// layer. accept() returns false, and stays false (latched), once the
// cumulative total exceeds the cap.
class ResponseByteCounter {
 public:
  explicit ResponseByteCounter(std::size_t cap) : cap_(cap) {}
  bool accept(int current, int total);

 private:
  std::size_t cap_;
  std::size_t banked_ = 0;  // bytes of completed readBytes calls (chunks)
  std::size_t last_ = 0;    // last `current` seen, to detect the per-chunk reset
  int last_total_ = -1;     // last `total` seen; a change also marks a new call
  bool exceeded_ = false;
};

// Cross-thread cancellation handle for the blocking calls below: abort() makes
// an in-flight postOffer/fetchPathsList return promptly with kNetwork (the
// connect phase included — ixwebsocket polls the cancel flag there too), and
// any later call passed the same handle fail fast. Latched: create a fresh one
// per connect attempt / fetch session.
class HttpAbort {
 public:
  HttpAbort();
  ~HttpAbort();
  HttpAbort(const HttpAbort&) = delete;
  HttpAbort& operator=(const HttpAbort&) = delete;

  void abort();
  bool aborted() const;

  struct Impl;  // internal to whep_client.cpp: tracks live ix request args
  Impl& impl() {
    return *impl_;
  }

 private:
  std::unique_ptr<Impl> impl_;
};
using HttpAbortPtr = std::shared_ptr<HttpAbort>;

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

// Timeout is clamped to >= 1s.
[[nodiscard]] PJ::Expected<WhepResult, WhepError> postOffer(
    const std::string& whep_url, const std::string& bearer_token, const std::string& offer_sdp,
    std::chrono::seconds timeout, const HttpAbortPtr& abort = nullptr);

// Best-effort session teardown; errors are ignored (the server reaps stale
// sessions on ICE timeout anyway). Timeout is clamped to >= 1s.
void deleteSession(const std::string& session_url, const std::string& bearer_token, std::chrono::seconds timeout);

// GET <api_url>/v3/paths/list and parse items[].{name,ready,tracks}. Timeout is clamped to >= 1s.
// The abort handle is also re-checked between pagination pages.
[[nodiscard]] PJ::Expected<std::vector<WhepPathInfo>, WhepError> fetchPathsList(
    const std::string& api_url, const std::string& bearer_token, std::chrono::seconds timeout,
    const HttpAbortPtr& abort = nullptr);

}  // namespace webrtc
}  // namespace PJ
