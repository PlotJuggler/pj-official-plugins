// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "whep_client.hpp"

namespace PJ {
namespace webrtc {

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
  (void)whep_url;
  (void)bearer_token;
  (void)offer_sdp;
  (void)timeout;
  return PJ::unexpected(WhepError{WhepErrorKind::kNetwork, "not implemented"});
}

void deleteSession(const std::string& session_url, const std::string& bearer_token, std::chrono::seconds timeout) {
  (void)session_url;
  (void)bearer_token;
  (void)timeout;
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
