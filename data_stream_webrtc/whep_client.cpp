// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "whep_client.hpp"

namespace PJ {
namespace webrtc {

std::string buildWhepUrl(const std::string& server_url, const std::string& path) {
  (void)server_url;
  (void)path;
  return {};
}

std::string resolveLocation(const std::string& request_url, const std::string& location) {
  (void)request_url;
  (void)location;
  return {};
}

WhepExpected<WhepResult> postOffer(
    const std::string& whep_url, const std::string& bearer_token, const std::string& offer_sdp, int timeout_sec) {
  (void)whep_url;
  (void)bearer_token;
  (void)offer_sdp;
  (void)timeout_sec;
  return {};
}

void deleteSession(const std::string& session_url, const std::string& bearer_token, int timeout_sec) {
  (void)session_url;
  (void)bearer_token;
  (void)timeout_sec;
}

WhepExpected<std::vector<WhepPathInfo>> fetchPathsList(
    const std::string& api_url, const std::string& bearer_token, int timeout_sec) {
  (void)api_url;
  (void)bearer_token;
  (void)timeout_sec;
  return {};
}

}  // namespace webrtc
}  // namespace PJ
