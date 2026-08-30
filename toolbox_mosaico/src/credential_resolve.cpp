// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "credential_resolve.hpp"

#include <pj_base/sdk/platform.hpp>

#include "core/origin_match.h"
#include "server_history.h"

namespace mosaico {

std::string credentialsSettingsPrefix(const std::string& uri) {
  return "mosaico/server_cache/" + normalizeServerKey(uri) + "/";
}

ServerCredentials loadCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri) {
  const std::string prefix = credentialsSettingsPrefix(uri);
  ServerCredentials creds;
  // Missing/unbound settings and backend read failures all leave the
  // credential defaults in place; every fallback is explicit at its read.
  if (auto stored = view.value(prefix + "cert_path")) {
    creds.cert_path = stored->toString();
  }
  if (auto stored = view.value(prefix + "api_key")) {
    creds.api_key = stored->toString();
  }
  if (auto stored = view.value(prefix + "allow_insecure")) {
    creds.allow_insecure = stored->toBool(false);
  }
  return creds;
}

void saveCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri, const ServerCredentials& creds) {
  const std::string prefix = credentialsSettingsPrefix(uri);
  // Persistence is best-effort; connection behavior uses the caller's
  // in-memory credentials even if the optional settings backend rejects it.
  (void)view.setValue(prefix + "cert_path", creds.cert_path);
  (void)view.setValue(prefix + "api_key", creds.api_key);
  (void)view.setValue(prefix + "allow_insecure", creds.allow_insecure);
}

ServerCredentials resolveCredentials(PJ::sdk::SettingsView view, const std::string& uri) {
  ServerCredentials creds = loadCredentialsForUri(view, uri);
  if (creds.api_key.empty()) {
    if (auto env = PJ::sdk::getEnv("MOSAICO_API_KEY")) {
      creds.api_key = *env;
    }
  }
  return creds;
}

bool envKeyAllowedForTarget(const std::string& target_uri, const std::string& mosaico_url_env) {
  return !mosaico_url_env.empty() && sameGrpcOrigin(target_uri, mosaico_url_env);
}

ServerCredentials resolveHeadlessCredentials(PJ::sdk::SettingsView view, const std::string& uri) {
  ServerCredentials creds = loadCredentialsForUri(view, uri);
  if (!creds.api_key.empty() && uri.rfind("grpc://", 0) == 0 && !creds.allow_insecure) {
    // normalizeServerKey deliberately aliases grpc and grpc+tls storage keys.
    // A layout must not turn that convenience into a credential downgrade.
    creds.api_key.clear();
  }
  if (creds.api_key.empty()) {
    const std::string url_env = PJ::sdk::getEnv("MOSAICO_URL").value_or("");
    if (envKeyAllowedForTarget(uri, url_env)) {
      if (auto env = PJ::sdk::getEnv("MOSAICO_API_KEY")) {
        creds.api_key = *env;
      }
    }
  }
  return creds;
}

}  // namespace mosaico
