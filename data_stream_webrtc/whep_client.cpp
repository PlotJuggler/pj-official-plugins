// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

// Must precede any include that can pull in <windows.h> (ixwebsocket does on
// MSVC): otherwise windows.h defines min/max macros that break std::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "whep_client.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace PJ {
namespace webrtc {

struct HttpAbort::Impl {
  std::atomic<bool> aborted{false};
  std::mutex mutex;
  // weak_ptr: an args object only stays reachable while its HTTP call runs;
  // holding it strongly would leak the callback->args cycle makeArgs avoids.
  std::vector<std::weak_ptr<ix::HttpRequestArgs>> live;

  void registerArgs(const ix::HttpRequestArgsPtr& args) {
    std::lock_guard<std::mutex> lk(mutex);
    live.erase(std::remove_if(live.begin(), live.end(), [](const auto& w) { return w.expired(); }), live.end());
    live.push_back(args);
    if (aborted.load()) {
      args->cancel = true;  // abort() raced this registration
    }
  }

  void abortAll() {
    aborted.store(true);
    std::lock_guard<std::mutex> lk(mutex);
    for (auto& w : live) {
      if (auto args = w.lock()) {
        // args->cancel is std::atomic<bool>, polled by ixwebsocket's connect
        // AND transfer loops — safe and effective from any thread.
        args->cancel = true;
      }
    }
  }
};

namespace {

// DoS hardening: refuse to buffer more than this per HTTP response (SDP answers
// and paths-list pages are tiny; anything bigger is not a WHEP/mediamtx peer).
constexpr std::size_t kMaxHttpResponseBytes = 8 * 1024 * 1024;

// Reject tokens carrying CR/LF/space/control bytes that could smuggle extra
// HTTP headers into the Authorization line.
bool bearerTokenSane(const std::string& t) {
  return std::none_of(t.begin(), t.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x21 || u == 0x7F;
  });
}

// Media type of a Content-Type header value: everything before the first ';',
// space-trimmed and lowercased (RFC 9110: type is case-insensitive and may
// carry parameters like "; charset=utf-8").
std::string mediaTypeOf(const std::string& content_type) {
  std::string t = content_type.substr(0, content_type.find(';'));
  const auto is_space = [](char c) { return c == ' ' || c == '\t'; };
  while (!t.empty() && is_space(t.front())) {
    t.erase(t.begin());
  }
  while (!t.empty() && is_space(t.back())) {
    t.pop_back();
  }
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return t;
}

// scheme://host[:port] — everything before the first '/' after "://".
// Empty string if the URL is not absolute.
std::string originOf(const std::string& url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return std::string();
  }
  const size_t path_start = url.find('/', scheme_end + 3);
  return (path_start == std::string::npos) ? url : url.substr(0, path_start);
}

ix::HttpRequestArgsPtr makeArgs(
    ix::HttpClient& client, const std::string& bearer_token, std::chrono::seconds timeout, const HttpAbortPtr& abort) {
  auto args = client.createRequest();
  const int secs = std::max(1, static_cast<int>(timeout.count()));
  args->connectTimeout = secs;
  args->transferTimeout = secs;
  args->followRedirects = false;
  // Response size cap, cumulative across chunked reads (ResponseByteCounter:
  // Socket::readBytes restarts `current` at every chunk). ixwebsocket 11.4.6
  // IGNORES the callback's bool in the HTTP receive path, so returning false
  // alone would not stop anything: flip args->cancel, which the transfer
  // loop's cancellation check does honor (surfaces as a transport error ->
  // kNetwork). Raw pointer, not the shared_ptr, to avoid an
  // args->callback->args ownership cycle.
  ix::HttpRequestArgs* raw_args = args.get();
  args->onProgressCallback = [raw_args, counter = ResponseByteCounter(kMaxHttpResponseBytes)](
                                 int current, int total) mutable -> bool {
    const bool within_cap = counter.accept(current, total);
    if (!within_cap) {
      raw_args->cancel = true;
    }
    return within_cap;
  };
  if (abort) {
    abort->impl().registerArgs(args);
  }
  if (!bearer_token.empty()) {
    args->extraHeaders["Authorization"] = "Bearer " + bearer_token;
  }
  return args;
}

WhepError errorFromResponse(const ix::HttpResponsePtr& res) {
  if (!res || res->errorCode != ix::HttpErrorCode::Ok) {
    const bool timeout = res && res->errorCode == ix::HttpErrorCode::Timeout;
    return WhepError{timeout ? WhepErrorKind::kTimeout : WhepErrorKind::kNetwork, res ? res->errorMsg : "no response"};
  }
  if (res->statusCode == 401 || res->statusCode == 403) {
    return WhepError{WhepErrorKind::kUnauthorized, "HTTP " + std::to_string(res->statusCode)};
  }
  if (res->statusCode == 404) {
    return WhepError{WhepErrorKind::kNotFound, "HTTP 404 (no publisher on this path yet?)"};
  }
  return WhepError{WhepErrorKind::kBadResponse, "HTTP " + std::to_string(res->statusCode)};
}

}  // namespace

bool ResponseByteCounter::accept(int current, int total) {
  if (exceeded_ || current < 0) {
    exceeded_ = true;
    return false;
  }
  const auto cur = static_cast<std::size_t>(current);
  // A new readBytes call (= next chunk) restarts `current` and usually changes
  // `total` (this call's read length): either signal banks the previous call.
  if (cur < last_ || total != last_total_) {
    banked_ += last_;
  }
  last_ = cur;
  last_total_ = total;
  exceeded_ = banked_ + cur > cap_;
  return !exceeded_;
}

HttpAbort::HttpAbort() : impl_(std::make_unique<Impl>()) {}
HttpAbort::~HttpAbort() = default;

void HttpAbort::abort() {
  impl_->abortAll();
}

bool HttpAbort::aborted() const {
  return impl_->aborted.load();
}

std::string buildWhepUrl(const std::string& server_url, const std::string& path) {
  std::string base = server_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  std::string p = path;
  while (!p.empty() && p.front() == '/') {
    p.erase(p.begin());
  }
  return base + "/" + p + "/whep";
}

std::string resolveLocation(const std::string& request_url, const std::string& location) {
  if (location.find("://") != std::string::npos) {
    return location;  // already absolute
  }
  const size_t scheme_end = request_url.find("://");
  if (scheme_end == std::string::npos) {
    return location;  // malformed request url; nothing better to do
  }
  const size_t host_start = scheme_end + 3;
  const size_t path_start = request_url.find('/', host_start);
  if (!location.empty() && location.front() == '/') {
    const std::string origin = (path_start == std::string::npos) ? request_url : request_url.substr(0, path_start);
    return origin + location;
  }
  // Path-relative: replace the last segment of the request URL's path.
  const size_t last_slash = request_url.rfind('/');
  if (last_slash == std::string::npos || last_slash < host_start) {
    return request_url + "/" + location;
  }
  return request_url.substr(0, last_slash + 1) + location;
}

PJ::Expected<WhepResult, WhepError> postOffer(
    const std::string& whep_url, const std::string& bearer_token, const std::string& offer_sdp,
    std::chrono::seconds timeout, const HttpAbortPtr& abort) {
  if (abort && abort->aborted()) {
    return PJ::unexpected(WhepError{WhepErrorKind::kNetwork, "aborted"});
  }
  if (!bearer_token.empty() && !bearerTokenSane(bearer_token)) {
    return PJ::unexpected(WhepError{WhepErrorKind::kUnauthorized, "invalid bearer token"});
  }
  ix::HttpClient client;
  auto args = makeArgs(client, bearer_token, timeout, abort);
  args->extraHeaders["Content-Type"] = "application/sdp";
  auto res = client.post(whep_url, offer_sdp, args);
  if (!res || res->errorCode != ix::HttpErrorCode::Ok || (res->statusCode != 201 && res->statusCode != 200)) {
    return PJ::unexpected(errorFromResponse(res));
  }
  if (res->body.size() > kMaxHttpResponseBytes) {  // belt-and-braces behind the makeArgs cap
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "response body too large"});
  }
  // Lenient on an ABSENT Content-Type (non-mediamtx servers), strict on a wrong
  // one: exact media-type match, case-insensitive, parameters ignored.
  const auto ctype = res->headers.find("Content-Type");
  if (ctype != res->headers.end() && mediaTypeOf(ctype->second) != "application/sdp") {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "unexpected answer Content-Type: " + ctype->second});
  }
  if (res->body.empty()) {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "empty answer SDP"});
  }
  const auto loc = res->headers.find("Location");
  if (loc == res->headers.end() || loc->second.empty()) {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "missing Location header"});
  }
  // Same-origin guard: an absolute Location may only point back at the server
  // we posted to; anything else smells like an open-redirect / token-stealing
  // response (the bearer token would be sent to the session URL on DELETE).
  if (loc->second.find("://") != std::string::npos && originOf(loc->second) != originOf(whep_url)) {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "cross-origin session URL rejected"});
  }
  WhepResult result;
  result.answer_sdp = res->body;
  result.session_url = resolveLocation(whep_url, loc->second);
  return result;
}

void deleteSession(const std::string& session_url, const std::string& bearer_token, std::chrono::seconds timeout) {
  if (!bearer_token.empty() && !bearerTokenSane(bearer_token)) {
    return;  // refuse to send a header-injecting token anywhere
  }
  ix::HttpClient client;
  auto args = makeArgs(client, bearer_token, timeout, nullptr);
  (void)client.request(session_url, "DELETE", "", args);
}

PJ::Expected<std::vector<WhepPathInfo>, WhepError> fetchPathsList(
    const std::string& api_url, const std::string& bearer_token, std::chrono::seconds timeout,
    const HttpAbortPtr& abort) {
  if (!bearer_token.empty() && !bearerTokenSane(bearer_token)) {
    return PJ::unexpected(WhepError{WhepErrorKind::kUnauthorized, "invalid bearer token"});
  }
  std::string base = api_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  const std::string list_url = base + "/v3/paths/list";
  // mediamtx paginates (100 items/page by default); page 0 tells us pageCount.
  // Discovery is best-effort for a picker UI: bound both the page count and
  // the cumulative item count — truncation is acceptable, unbounded fetching
  // from a hostile/misconfigured server is not.
  constexpr int kMaxPages = 10;
  constexpr std::size_t kMaxTotalItems = 2000;
  int page_count = 1;
  std::vector<WhepPathInfo> paths;
  for (int page = 0; page < page_count && page < kMaxPages; ++page) {
    // Re-checked per page: a dialog closing mid-fetch must not sit through the
    // remaining pages' connect/transfer timeouts.
    if (abort && abort->aborted()) {
      return PJ::unexpected(WhepError{WhepErrorKind::kNetwork, "aborted"});
    }
    ix::HttpClient client;
    auto args = makeArgs(client, bearer_token, timeout, abort);
    auto res = client.get(list_url + "?page=" + std::to_string(page), args);
    if (!res || res->errorCode != ix::HttpErrorCode::Ok || res->statusCode != 200) {
      return PJ::unexpected(errorFromResponse(res));  // no partial results
    }
    if (res->body.size() > kMaxHttpResponseBytes) {  // belt-and-braces behind the makeArgs cap
      return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "response body too large"});
    }
    const auto doc = nlohmann::json::parse(res->body, nullptr, false);
    if (doc.is_discarded() || !doc.contains("items") || !doc["items"].is_array()) {
      return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "unexpected /v3/paths/list JSON"});
    }
    if (page == 0 && doc.contains("pageCount") && doc["pageCount"].is_number_integer()) {
      page_count = doc["pageCount"].get<int>();
    }
    for (const auto& item : doc["items"]) {
      if (paths.size() >= kMaxTotalItems) {
        break;  // cumulative cap reached: stop collecting
      }
      if (!item.is_object()) {
        continue;  // skip malformed entries rather than throwing (untrusted input)
      }
      try {
        WhepPathInfo info;
        info.name = item.value("name", std::string());
        info.ready = item.value("ready", false);
        if (item.contains("tracks") && item["tracks"].is_array()) {
          for (const auto& t : item["tracks"]) {
            if (t.is_string()) {
              info.tracks.push_back(t.get<std::string>());
            }
          }
        }
        if (!info.name.empty()) {
          paths.push_back(std::move(info));
        }
      } catch (const nlohmann::json::exception&) {
        continue;  // wrong-typed field: skip this entry, keep the rest
      }
    }
    if (paths.size() >= kMaxTotalItems) {
      break;  // cumulative cap reached: stop fetching further pages
    }
  }
  return paths;
}

}  // namespace webrtc
}  // namespace PJ
