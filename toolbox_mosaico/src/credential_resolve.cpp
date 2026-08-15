// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "credential_resolve.hpp"

#include <pj_base/sdk/platform.hpp>

#include "core/origin_match.h"
#include "server_history.h"
#include "settings_store.hpp"

namespace mosaico {

std::string credentialsSettingsPrefix(const std::string& uri) {
  return "mosaico/server_cache/" + normalizeServerKey(uri) + "/";
}

ServerCredentials loadCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri) {
  SettingsStore settings(view);
  const std::string prefix = credentialsSettingsPrefix(uri);
  ServerCredentials creds;
  creds.cert_path = settings.getString(prefix + "cert_path");
  creds.api_key = settings.getString(prefix + "api_key");
  creds.allow_insecure = settings.getBool(prefix + "allow_insecure", false);
  return creds;
}

void saveCredentialsForUri(PJ::sdk::SettingsView view, const std::string& uri, const ServerCredentials& creds) {
  SettingsStore settings(view);
  const std::string prefix = credentialsSettingsPrefix(uri);
  settings.setString(prefix + "cert_path", creds.cert_path);
  settings.setString(prefix + "api_key", creds.api_key);
  settings.setBool(prefix + "allow_insecure", creds.allow_insecure);
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
