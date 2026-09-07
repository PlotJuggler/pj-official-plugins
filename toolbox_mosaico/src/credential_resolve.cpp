// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "credential_resolve.hpp"

#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/source/origin.hpp>

#include "server_history.h"
#include "settings_store.hpp"

namespace mosaico {

namespace {

const PJ::sdk::source::OriginPolicy& grpcOriginPolicy() {
  static const PJ::sdk::source::OriginPolicy policy{{"grpc", "grpc+tls"}, {}};
  return policy;
}

}  // namespace

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
  return !mosaico_url_env.empty() && PJ::sdk::source::sameOrigin(target_uri, mosaico_url_env, grpcOriginPolicy());
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

std::string headlessTargetUri(const std::string& origin) {
  return "grpc+tls://" + origin;
}

std::optional<std::string> headlessPlaintextRetryUri(PJ::sdk::SettingsView view, const std::string& origin) {
  const ServerCredentials creds = loadCredentialsForUri(view, origin);
  if (!creds.allow_insecure || !creds.cert_path.empty()) {
    return std::nullopt;
  }
  return "grpc://" + origin;
}

}  // namespace mosaico
