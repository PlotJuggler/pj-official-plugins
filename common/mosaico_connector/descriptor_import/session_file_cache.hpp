// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Request-addressed session file cache core: identity->path mapping,
// cross-process exclusive materialization locking, validated atomic
// finalization, and orphan/LRU maintenance. Format-agnostic by construction:
// artifact validation is an injected callback (the Arrow-IPC artifact module
// supplies the real one), so the locking/rename/eviction mechanics are
// testable — and provably correct — independently of the artifact format.
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "descriptor_import/core/file_lock.h"

namespace mosaico {

/// The env-resolved standard cache root: MOSAICO_CACHE_DIR ||
/// $XDG_CACHE_HOME/mosaico/sessions || ~/.cache/mosaico/sessions. Empty (with
/// `error` set) when unresolvable. The dialog shows this as the
/// cache-directory placeholder hint (the stored setting stays empty unless
/// the user picks a directory, so this resolution stays live).
[[nodiscard]] std::filesystem::path standardCacheRoot(std::string* error);

/// Request-addressed session cache. Files: <root>/<128-bit-hex>.pjmosaico;
/// partials: <name>.pjmosaico.partial.<pid>; sidecars: <name>.pjmosaico.lock
/// (materialize/evict mutual exclusion and the read-lease point) and
/// <name>.pjmosaico.touch (LRU stamp — atime is unreliable under
/// relatime/noatime, so hits touch explicitly).
/// Root: MOSAICO_CACHE_DIR || $XDG_CACHE_HOME/mosaico/sessions ||
/// ~/.cache/mosaico/sessions. Directory 0700, files 0600.
class SessionFileCache {
 public:
  struct Config {
    std::uintmax_t max_total_bytes = 20ull * 1024 * 1024 * 1024;  // 20 GiB
    std::uintmax_t min_free_bytes = 2ull * 1024 * 1024 * 1024;    // reserve
    std::chrono::hours orphan_partial_age{24};
  };

  /// Artifact validation callback: given the file, the identity's 32-char hex
  /// digest, decide whether the file is a structurally valid artifact FOR
  /// THAT identity. Must be bounded I/O (lookup runs on the GUI thread) and
  /// must verify the embedded descriptor provenance by RE-HASHING its bytes —
  /// never by trusting a stored identity string. Fetch completeness is the
  /// producer's publication gate: incomplete fetches never call finalize().
  using Validator = std::function<bool(const std::filesystem::path& file, const std::string& hex, std::string* error)>;

  /// The validator is mandatory: a store cannot classify hits without one,
  /// and defaulting to "accept" would let any foreign file at <digest>.pjmosaico
  /// classify as a hit. Tests inject stubs; production injects the Arrow-IPC
  /// artifact validator.
  SessionFileCache(std::filesystem::path root, Validator validator);
  /// Env-resolved root (see class comment). On unresolvable root returns a
  /// store with an empty root — every operation then fails cleanly.
  static SessionFileCache standard(Validator validator, std::string* error);
  /// The ONE "configured directory else standard root" rule: a non-empty
  /// `configured_root` (the panel's cache-directory setting) wins; empty
  /// falls back to standard(). Every cache consumer (capture, provider query,
  /// import job) resolves through here so they can never disagree on where
  /// an identity's artifact lives.
  static SessionFileCache at(const std::filesystem::path& configured_root, Validator validator, std::string* error);

  /// Exclusive per-identity materialization guard (RAII; non-blocking).
  /// Holds the identity's sidecar .lock for its lifetime — finalize, orphan
  /// cleanup and eviction all contend on the same lock file.
  class MaterializeLock {
   public:
    MaterializeLock(MaterializeLock&&) noexcept = default;
    MaterializeLock& operator=(MaterializeLock&&) noexcept = default;

   private:
    friend class SessionFileCache;
    MaterializeLock(FileLock lock, std::string hex, std::filesystem::path partial);

    FileLock lock_;
    std::string hex_;  // the identity's 32-char lowercase-hex digest
    std::filesystem::path partial_;
  };

  /// identity = the full "mosaico:v1:sha256/128:<hex>" string (validated).
  /// Returns an empty path for anything malformed — a bad identity can never
  /// name a file outside the root.
  [[nodiscard]] std::filesystem::path pathFor(std::string_view identity) const;

  /// Existing + validator-approved. Touches the LRU stamp on hit. A failed
  /// check is a plain miss — the file is NOT deleted here;
  /// re-materialization atomically renames over it.
  [[nodiscard]] bool lookup(std::string_view identity, std::filesystem::path* out);

  /// `contended` (optional out): true iff the failure is the lock being held
  /// by ANOTHER holder (any process — retry-able), false for OS/identity
  /// errors.
  [[nodiscard]] std::optional<MaterializeLock> tryLockForMaterialize(
      std::string_view identity, std::string* error, bool* contended = nullptr);

  /// Convert a finalized identity's EXCLUSIVE materialize lock into a SHARED
  /// read lease WITHOUT closing the underlying handle — the exclusive->shared
  /// handoff that keeps another process's cleanup from unlinking a
  /// just-published file that live datasets will lazily re-open. The platform
  /// conversion has a microscopic non-atomic window (see
  /// FileLock::downgradeToShared); on failure nullopt is returned with the
  /// lock RELEASED — the caller revalidates and proceeds lease-less. Call
  /// only AFTER finalize() succeeded.
  [[nodiscard]] static std::optional<FileLock> toSharedLease(MaterializeLock&& lock, std::string* error);

  /// The partial path this process must write under `lock`.
  [[nodiscard]] std::filesystem::path partialPathFor(const MaterializeLock& lock) const;

  /// Validate the finished partial through the injected validator, fsync
  /// file, atomic rename to pathFor(identity), fsync directory (POSIX). On
  /// any failure: remove the partial, return false with the reason. The
  /// producer calls this only after its fetch ledger reports completeness.
  [[nodiscard]] bool finalize(const MaterializeLock& lock, std::string* error);

  /// SHARED read lease on `identity`'s lock sidecar (every live cache-backed
  /// consumer holds one for the file's whole lifetime — Linux
  /// unlink-while-open does NOT protect lazy re-opens). While any lease is
  /// live, cleanup()/eviction and tryLockForMaterialize (both exclusive on
  /// the same sidecar) fail/skip; leases stack across holders. Pair with
  /// lookup() — this takes no position on whether the cache file exists.
  [[nodiscard]] std::optional<FileLock> acquireReadLease(std::string_view identity, std::string* error);

  /// Startup/maintenance: remove orphaned partials older than
  /// orphan_partial_age whose lock is free; then LRU-evict unlocked files
  /// (touch-file order) until under max_total_bytes AND min_free_bytes holds.
  /// Skips any file whose identity lock is busy — a live materialization OR a
  /// shared read lease (acquireReadLease) both hold that lock.
  void cleanup(const Config& cfg);

 private:
  std::filesystem::path root_;
  Validator validator_;
};

}  // namespace mosaico
