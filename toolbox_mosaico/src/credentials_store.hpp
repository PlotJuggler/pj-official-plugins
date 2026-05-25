// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QStringList>

namespace mosaico {

/// Per-server credential bundle stored in QSettings under
/// `mosaico/server_cache/<normalized_key>/`. Key normalization is delegated
/// to server_history::normalizeServerKey.
struct ServerCredentials {
  QString cert_path;
  QString api_key;
  bool allow_insecure = false;
};

/// Load the credentials cached for `uri`. Returns defaults if not cached.
[[nodiscard]] ServerCredentials loadCredentials(const QString& uri);

/// Persist credentials for `uri` (creates the cache entry on first save).
void saveCredentials(const QString& uri, const ServerCredentials& creds);

}  // namespace mosaico
