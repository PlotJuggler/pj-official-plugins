// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "whep_client.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <utility>

namespace PJ {
namespace webrtc {

namespace {

ix::HttpRequestArgsPtr makeArgs(ix::HttpClient& client, const std::string& bearer_token, std::chrono::seconds timeout) {
  auto args = client.createRequest();
  args->connectTimeout = static_cast<int>(timeout.count());
  args->transferTimeout = static_cast<int>(timeout.count());
  args->followRedirects = false;
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
    std::chrono::seconds timeout) {
  ix::HttpClient client;
  auto args = makeArgs(client, bearer_token, timeout);
  args->extraHeaders["Content-Type"] = "application/sdp";
  auto res = client.post(whep_url, offer_sdp, args);
  if (!res || res->errorCode != ix::HttpErrorCode::Ok || (res->statusCode != 201 && res->statusCode != 200)) {
    return PJ::unexpected(errorFromResponse(res));
  }
  if (res->body.empty()) {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "empty answer SDP"});
  }
  const auto loc = res->headers.find("Location");
  if (loc == res->headers.end() || loc->second.empty()) {
    return PJ::unexpected(WhepError{WhepErrorKind::kBadResponse, "missing Location header"});
  }
  WhepResult result;
  result.answer_sdp = res->body;
  result.session_url = resolveLocation(whep_url, loc->second);
  return result;
}

void deleteSession(const std::string& session_url, const std::string& bearer_token, std::chrono::seconds timeout) {
  ix::HttpClient client;
  auto args = makeArgs(client, bearer_token, timeout);
  (void)client.request(session_url, "DELETE", "", args);
}

PJ::Expected<std::vector<WhepPathInfo>, WhepError> fetchPathsList(
    const std::string& api_url, const std::string& bearer_token, std::chrono::seconds timeout) {
  (void)api_url;
  (void)bearer_token;
  (void)timeout;
  return PJ::unexpected(WhepError{WhepErrorKind::kNetwork, "not implemented"});
}

}  // namespace webrtc
}  // namespace PJ
