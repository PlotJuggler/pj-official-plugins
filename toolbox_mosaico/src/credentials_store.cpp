// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "credentials_store.hpp"

#include <QSettings>

#include "server_history.h"

namespace mosaico {

namespace {

QString settingsPrefix(const QString& uri) {
  return QStringLiteral("mosaico/server_cache/") + normalizeServerKey(uri) + QStringLiteral("/");
}

}  // namespace

ServerCredentials loadCredentials(const QString& uri) {
  QSettings settings;
  const QString prefix = settingsPrefix(uri);
  ServerCredentials creds;
  creds.cert_path = settings.value(prefix + QStringLiteral("cert_path")).toString();
  creds.api_key = settings.value(prefix + QStringLiteral("api_key")).toString();
  creds.allow_insecure = settings.value(prefix + QStringLiteral("allow_insecure"), false).toBool();
  return creds;
}

void saveCredentials(const QString& uri, const ServerCredentials& creds) {
  QSettings settings;
  const QString prefix = settingsPrefix(uri);
  settings.setValue(prefix + QStringLiteral("cert_path"), creds.cert_path);
  settings.setValue(prefix + QStringLiteral("api_key"), creds.api_key);
  settings.setValue(prefix + QStringLiteral("allow_insecure"), creds.allow_insecure);
}

}  // namespace mosaico
