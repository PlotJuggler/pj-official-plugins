// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Minimal RAII advisory file lock over a dedicated sidecar lock file (POSIX
// flock / Windows LockFileEx), non-blocking try-acquire only. Two modes:
// EXCLUSIVE (materialization/eviction/cleanup mutual exclusion) and SHARED
// (read leases: live cache-backed datasets pinning their file against
// eviction). Shared holders stack;
// any shared holder blocks an exclusive try and vice versa — flock(LOCK_SH)
// on POSIX, LockFileEx WITHOUT LOCKFILE_EXCLUSIVE_LOCK on Windows (both lock
// the same byte range, so the two modes contend correctly cross-platform).
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mosaico {

class FileLock {
 public:
  /// Non-blocking try-acquire of an exclusive advisory lock on `path`,
  /// creating the lock file 0600 if absent. Returns nullopt with `*error`
  /// set when the lock is held elsewhere (any process, INCLUDING this one via
  /// a different FileLock) or when the OS call fails. The lock file itself is
  /// never deleted: unlinking a path another process may be about to open
  /// would hand out two "exclusive" locks on different inodes.
  /// `contended` (optional out, adversarial F9): true when the failure is
  /// the lock being HELD elsewhere (retry-able), false for any OS error
  /// (not retry-able) — callers implementing bounded waits must distinguish.
  [[nodiscard]] static std::optional<FileLock> tryExclusive(
      const std::filesystem::path& path, std::string* error, bool* contended = nullptr);

  /// Non-blocking try-acquire of a SHARED (read-lease) advisory lock on
  /// `path`, creating the lock file 0600 if absent. Multiple shared holders
  /// coexist (across processes and within one); the try fails while an
  /// exclusive holder is live, and any live shared holder makes a concurrent
  /// tryExclusive fail — the eviction/materialization skip contract.
  [[nodiscard]] static std::optional<FileLock> tryShared(const std::filesystem::path& path, std::string* error);

  /// FOLLOW-UP (recorded, round-5 F2 option (b)): immutable GENERATION-
  /// SUFFIXED artifact paths + per-generation leases — each materialization
  /// writes <digest>.<gen>.arrows and readers pin their own generation, which
  /// removes rename-over entirely (no refusal needed, cross-process
  /// referenced identities become first-class) at the cost of a cache-layout
  /// migration. The refusal-while-referenced rule implemented now is the
  /// interim.
  ///
  /// FOLLOW-UP (recorded, re-verify R1): migrate FileLock to fcntl OFD
  /// locks (F_OFD_SETLK), whose lock CONVERSIONS are atomic — that closes
  /// downgradeToShared()'s non-atomic window for real. It must be a
  /// whole-release migration: flock(2) and fcntl locks live in independent
  /// namespaces, so every acquirer (cache sidecars, ledger lock) has to
  /// switch in the same release, plus a Windows byte-range equivalent.
  ///
  /// Convert a HELD EXCLUSIVE lock to SHARED without closing the handle
  /// (the adversarial-F2 finalize handoff). Platform reality: neither
  /// flock(2) nor LockFileEx guarantees an atomic conversion — the old lock
  /// can drop before the new one lands, so a concurrent non-blocking
  /// exclusive try may slip into that microscopic window. On conversion
  /// failure the lock is RELEASED and false is returned; the caller must
  /// revalidate whatever the lock protected and proceed lease-less.
  [[nodiscard]] bool downgradeToShared(std::string* error);

  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  /// Releases the lock: close(2) drops the flock; UnlockFileEx + CloseHandle
  /// on Windows.
  ~FileLock();

 private:
  explicit FileLock(std::intptr_t handle) : handle_(handle) {}
  void release();

  // POSIX fd / Windows HANDLE. -1 = released/moved-from (INVALID_HANDLE_VALUE
  // is (HANDLE)-1, so one sentinel serves both).
  std::intptr_t handle_ = -1;
};

}  // namespace mosaico
