// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "descriptor_import/session_file_cache.hpp"

#include <algorithm>
#include <fstream>
#include <pj_base/sdk/platform.hpp>
#include <system_error>
#include <vector>

#include "descriptor_import/core/fs_durability.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mosaico {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kIdentityPrefix = "mosaico:v1:sha256/128:";
constexpr std::size_t kDigestHexChars = 32;  // 128 bits, lowercase hex
// The artifact is an MCAP container inside (see arrow_cache_artifact.hpp),
// but carries the .pjmosaico extension: the host resolves even a
// pinned-by-id loader through its declared file extensions, so the cache
// loader's extension and the artifact's must match — and neither may be
// ".mcap", or every ordinary MCAP drag-drop would see two candidate
// loaders. The mcap CLI reads the file regardless of extension.
constexpr const char* kArtifactSuffix = ".pjmosaico";

// Extract the digest component from a full identity string; nullopt for
// anything that is not EXACTLY "mosaico:v1:sha256/128:<32 lowercase hex>".
std::optional<std::string> identityHex(std::string_view identity) {
  if (identity.size() != kIdentityPrefix.size() + kDigestHexChars) {
    return std::nullopt;
  }
  if (identity.substr(0, kIdentityPrefix.size()) != kIdentityPrefix) {
    return std::nullopt;
  }
  const std::string_view hex = identity.substr(kIdentityPrefix.size());
  for (const char c : hex) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) {
      return std::nullopt;
    }
  }
  return std::string(hex);
}

fs::path touchPathFor(const fs::path& file) {
  return fs::path(file.string() + ".touch");
}

fs::path lockPathFor(const fs::path& file) {
  return fs::path(file.string() + ".lock");
}

// Update (or create) the LRU stamp beside `file`. The sidecar's mtime is the
// eviction order; lookup hits and finalization both move it to now.
void touchStamp(const fs::path& file) {
  const fs::path stamp = touchPathFor(file);
  { std::ofstream out(stamp, std::ios::binary | std::ios::trunc); }
  std::error_code ec;
  fs::last_write_time(stamp, fs::file_time_type::clock::now(), ec);
  chmod0600(stamp);
}

}  // namespace

SessionFileCache::MaterializeLock::MaterializeLock(FileLock lock, std::string hex, fs::path partial)
    : lock_(std::move(lock)), hex_(std::move(hex)), partial_(std::move(partial)) {}

SessionFileCache::SessionFileCache(fs::path root, Validator validator)
    : root_(std::move(root)), validator_(std::move(validator)) {}

fs::path standardCacheRoot(std::string* error) {
  if (auto v = PJ::sdk::getEnv("MOSAICO_CACHE_DIR")) {
    return fs::path(*v);
  }
  if (auto v = PJ::sdk::getEnv("XDG_CACHE_HOME")) {
    return fs::path(*v) / "mosaico" / "sessions";
  }
  if (auto v = PJ::sdk::getEnv("HOME")) {
    return fs::path(*v) / ".cache" / "mosaico" / "sessions";
  }
  if (error) {
    *error = "cache root unresolvable (MOSAICO_CACHE_DIR, XDG_CACHE_HOME and HOME all unset)";
  }
  return {};
}

SessionFileCache SessionFileCache::standard(Validator validator, std::string* error) {
  return SessionFileCache(standardCacheRoot(error), std::move(validator));
}

SessionFileCache SessionFileCache::at(const fs::path& configured_root, Validator validator, std::string* error) {
  if (!configured_root.empty()) {
    return SessionFileCache(configured_root, std::move(validator));
  }
  return standard(std::move(validator), error);
}

fs::path SessionFileCache::pathFor(std::string_view identity) const {
  const auto hex = identityHex(identity);
  if (!hex.has_value() || root_.empty()) {
    return {};
  }
  return root_ / (*hex + kArtifactSuffix);
}

bool SessionFileCache::lookup(std::string_view identity, fs::path* out) {
  const auto hex = identityHex(identity);
  if (!hex.has_value() || root_.empty() || !validator_) {
    return false;
  }
  const fs::path file = root_ / (*hex + kArtifactSuffix);
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || ec) {
    return false;
  }
  std::string error;
  if (!validator_(file, *hex, &error)) {
    return false;
  }
  touchStamp(file);
  if (out) {
    *out = file;
  }
  return true;
}

std::optional<SessionFileCache::MaterializeLock> SessionFileCache::tryLockForMaterialize(
    std::string_view identity, std::string* error, bool* contended) {
  if (contended != nullptr) {
    *contended = false;
  }
  const auto hex = identityHex(identity);
  if (!hex.has_value()) {
    if (error) {
      *error = "invalid descriptor identity (want mosaico:v1:sha256/128:<32 lowercase hex>)";
    }
    return std::nullopt;
  }
  if (root_.empty()) {
    if (error) {
      *error = "cache root is not configured";
    }
    return std::nullopt;
  }
  ensureDir0700(root_);
  std::string lock_error;
  auto lock = FileLock::tryExclusive(root_ / (*hex + kArtifactSuffix + ".lock"), &lock_error, contended);
  if (!lock.has_value()) {
    if (error) {
      *error = "cache materialize lock unavailable: " + lock_error;
    }
    return std::nullopt;
  }
#if defined(_WIN32)
  const auto pid = static_cast<long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<long long>(::getpid());
#endif
  fs::path partial = root_ / (*hex + kArtifactSuffix + ".partial." + std::to_string(pid));
  return MaterializeLock(std::move(*lock), *hex, std::move(partial));
}

std::optional<FileLock> SessionFileCache::toSharedLease(MaterializeLock&& lock, std::string* error) {
  FileLock held = std::move(lock.lock_);
  if (!held.downgradeToShared(error)) {
    return std::nullopt;  // released; the caller must reacquire + revalidate
  }
  return held;
}

fs::path SessionFileCache::partialPathFor(const MaterializeLock& lock) const {
  return lock.partial_;
}

std::optional<FileLock> SessionFileCache::acquireReadLease(std::string_view identity, std::string* error) {
  const auto hex = identityHex(identity);
  if (!hex.has_value()) {
    if (error) {
      *error = "invalid descriptor identity (want mosaico:v1:sha256/128:<32 lowercase hex>)";
    }
    return std::nullopt;
  }
  if (root_.empty()) {
    if (error) {
      *error = "cache root is not configured";
    }
    return std::nullopt;
  }
  ensureDir0700(root_);
  std::string lock_error;
  auto lease = FileLock::tryShared(root_ / (*hex + kArtifactSuffix + ".lock"), &lock_error);
  if (!lease.has_value()) {
    if (error) {
      *error = "cache read lease unavailable: " + lock_error;
    }
    return std::nullopt;
  }
  return lease;
}

bool SessionFileCache::finalize(const MaterializeLock& lock, std::string* error) {
  const fs::path& partial = lock.partial_;
  const fs::path final_path = root_ / (lock.hex_ + kArtifactSuffix);
  const auto fail = [&](const std::string& reason) {
    std::error_code ec;
    fs::remove(partial, ec);
    if (error) {
      *error = reason;
    }
    return false;
  };
  if (!validator_) {
    return fail("cache finalize rejected " + partial.filename().string() + ": no artifact validator configured");
  }
  std::string reason;
  if (!validator_(partial, lock.hex_, &reason)) {
    return fail("cache finalize rejected " + partial.filename().string() + ": " + reason);
  }
  chmod0600(partial);
  if (!syncFile(partial, &reason)) {
    return fail(reason);
  }
  std::error_code ec;
  fs::rename(partial, final_path, ec);
  if (ec) {
    return fail("atomic rename failed: " + ec.message());
  }
  (void)syncDir(root_);
  touchStamp(final_path);
  return true;
}

void SessionFileCache::cleanup(const Config& cfg) {
  std::error_code ec;
  if (root_.empty() || !fs::is_directory(root_, ec) || ec) {
    return;
  }

  // Pass 1: orphaned partials — old enough AND identity lock free. BOTH
  // guards matter: flock dies with its process (so a crashed writer's partial
  // becomes collectable), while the age threshold keeps process B from ever
  // deleting process A's live partial in a lock-handoff instant.
  const std::string partial_marker = std::string(kArtifactSuffix) + ".partial.";
  const auto now = fs::file_time_type::clock::now();
  for (const auto& entry : fs::directory_iterator(root_, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const auto partial_pos = name.find(partial_marker);
    if (partial_pos == std::string::npos) {
      continue;
    }
    const auto mtime = fs::last_write_time(entry.path(), entry_ec);
    if (entry_ec || now - mtime < cfg.orphan_partial_age) {
      continue;
    }
    const std::string base = name.substr(0, partial_pos) + kArtifactSuffix;
    std::string lock_error;
    const auto lock = FileLock::tryExclusive(root_ / (base + ".lock"), &lock_error);
    if (!lock.has_value()) {
      continue;  // a live materialization owns this identity
    }
    fs::remove(entry.path(), entry_ec);
  }

  // Pass 2: LRU eviction by touch-stamp order until BOTH budgets hold. Each
  // victim's identity lock is taken non-blocking first — busy means a live
  // materialization or a shared dataset lease: skip it.
  struct Candidate {
    fs::path file;
    std::uintmax_t size;
    fs::file_time_type stamp;
  };
  std::vector<Candidate> candidates;
  std::uintmax_t total = 0;
  for (const auto& entry : fs::directory_iterator(root_, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    if (!entry.path().filename().string().ends_with(kArtifactSuffix)) {
      continue;  // partials, .touch and .lock sidecars are not evictable
    }
    const std::uintmax_t size = entry.file_size(entry_ec);
    if (entry_ec) {
      continue;
    }
    total += size;
    auto stamp = fs::last_write_time(touchPathFor(entry.path()), entry_ec);
    if (entry_ec) {
      // No touch sidecar (pre-stamp file or a deleted stamp): fall back to
      // the file's own mtime so it still participates in the order.
      stamp = fs::last_write_time(entry.path(), entry_ec);
      if (entry_ec) {
        stamp = fs::file_time_type::min();
      }
    }
    candidates.push_back({entry.path(), size, stamp});
  }
  std::sort(
      candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.stamp < b.stamp; });
  const auto overBudget = [&] {
    if (total > cfg.max_total_bytes) {
      return true;
    }
    std::error_code space_ec;
    const fs::space_info space = fs::space(root_, space_ec);
    return !space_ec && space.available < cfg.min_free_bytes;
  };
  for (const Candidate& victim : candidates) {
    if (!overBudget()) {
      break;
    }
    std::string lock_error;
    const auto lock = FileLock::tryExclusive(lockPathFor(victim.file), &lock_error);
    if (!lock.has_value()) {
      continue;  // leased or re-materializing: never evict under a holder
    }
    std::error_code remove_ec;
    if (!fs::remove(victim.file, remove_ec) || remove_ec) {
      continue;
    }
    total -= victim.size;
    fs::remove(touchPathFor(victim.file), remove_ec);
  }
}

}  // namespace mosaico
