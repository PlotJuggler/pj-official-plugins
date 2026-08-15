// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Trusted-origin ledger for layout re-import: *trusted = origin recorded
// after a successful INTERACTIVE connect in the Mosaico panel*. This is a
// dedicated ledger, deliberately NOT the credential cache — a stored api key
// can exist before any successful connection, so "has a stored key" must
// never imply "safe to auto-import against".
//
// Storage format: {"v":1,"origins":["grpc+tls://host:6726", ...]} where each
// serialized origin is scheme://host:port (parseGrpcOrigin — the port is
// always explicit in a parsable Mosaico URI). File discipline: config root,
// 0700 dir, 0600 file, atomic replace, corrupt-file-reads-as-empty.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mosaico {

/// The per-user Mosaico config root: MOSAICO_CONFIG_DIR ||
/// $XDG_CONFIG_HOME/mosaico || ~/.config/mosaico (Windows: %APPDATA%/mosaico).
/// Empty when no root can be resolved — consumers then degrade cleanly
/// (nothing trusted, nothing recordable).
[[nodiscard]] std::filesystem::path defaultConfigRoot();

/// The canonical serialized trust key for `uri` — "scheme://host:port"
/// (the exact string the ledger stores). nullopt for shapes parseGrpcOrigin
/// rejects. Shared with the provider's in-memory trust set so the two can
/// never drift on normalization.
[[nodiscard]] std::optional<std::string> trustedOriginKey(std::string_view uri);

/// Ledger of origins that completed a successful interactive connect on this
/// machine (file: <config_root>/trusted_origins.json, 0600, atomic replace).
class TrustedOrigins {
 public:
  explicit TrustedOrigins(std::filesystem::path config_root);  // tests inject
  static TrustedOrigins standard();                            // defaultConfigRoot()

  /// Record the origin of `uri` (parseGrpcOrigin; false on unparsable).
  /// DURABLE-OR-FALSE: the whole read-modify-write is serialized across
  /// processes by an exclusive lock on <ledger>.lock (bounded retry), the
  /// ledger is re-read UNDER the lock, written to a unique temp, fsynced
  /// (file + directory) and atomically renamed. False = nothing durable
  /// happened; the caller must not treat the origin as trusted.
  [[nodiscard]] bool recordSuccessfulConnect(std::string_view uri);

  /// True iff `uri`'s origin was ever recorded. Re-reads the ledger file per
  /// call — bounded-query consumers preload allOrigins() into an in-memory
  /// set instead of calling this on a hot path.
  [[nodiscard]] bool isTrusted(std::string_view uri) const;

  /// Every recorded serialized origin (trustedOriginKey shape) — the
  /// provider's construction-time preload for its in-memory trust set.
  [[nodiscard]] std::vector<std::string> allOrigins() const;

 private:
  std::filesystem::path path_;
};

namespace testing {
/// Force the ledger's directory fsync to report failure (a real directory
/// whose fsync fails is not constructible portably from a test).
void setTrustedOriginsDirSyncFailForTest(bool fail);
}  // namespace testing

}  // namespace mosaico
