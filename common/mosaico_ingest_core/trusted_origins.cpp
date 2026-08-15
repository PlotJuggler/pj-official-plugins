// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "trusted_origins.hpp"

#include <chrono>
#include <fstream>
#include <optional>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <nlohmann/json.hpp>
#include <pj_base/sdk/platform.hpp>

#include "core/file_lock.h"
#include "core/fs_durability.h"
#include "core/origin_match.h"

namespace mosaico {

namespace fs = std::filesystem;

namespace {

std::string serializeOrigin(const Origin& origin) {
  return origin.scheme + "://" + origin.host + ":" + std::to_string(origin.port);
}

// Load the recorded-origins array from disk. A missing, unreadable, or
// malformed file — including a directory squatting on the path — reads as
// empty (never throws): the ledger degrades to "nothing trusted" and the
// next record replaces the bad content.
nlohmann::json loadOrigins(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || ec) {
    return nlohmann::json::array();
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return nlohmann::json::array();
  }
  nlohmann::json parsed = nlohmann::json::parse(in, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    return nlohmann::json::array();
  }
  auto it = parsed.find("origins");
  if (it == parsed.end() || !it->is_array()) {
    return nlohmann::json::array();
  }
  return *it;
}

bool g_fail_dir_sync_for_test = false;

// A rename whose directory entry never reached stable storage can vanish on
// crash, so a directory-fsync failure fails the whole write (the shared
// syncDir is a documented no-op success on Windows).
[[nodiscard]] bool syncLedgerDir(const fs::path& dir) {
  if (g_fail_dir_sync_for_test) {
    return false;
  }
  return syncDir(dir);
}

// Durably persist the ledger: UNIQUE temp (pid-suffixed — two processes must
// never share one), 0600, fsync, atomic rename, directory fsync. A rename
// failure is a FAILURE (a direct-overwrite fallback could be truncated
// mid-crash); false = nothing durable happened.
[[nodiscard]] bool writeOrigins(const fs::path& file, const nlohmann::json& origins) {
  ensureDir0700(file.parent_path());
  nlohmann::json obj;
  obj["v"] = 1;
  obj["origins"] = origins;
#if defined(_WIN32)
  const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const fs::path tmp = file.parent_path() / (file.filename().string() + ".tmp." + std::to_string(pid));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << obj.dump(2);
    out.flush();
    if (!out) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
  }
  chmod0600(tmp);
  if (!syncFile(tmp, nullptr)) {
    std::error_code ec;
    fs::remove(tmp, ec);
    return false;
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  chmod0600(file);
  if (!syncLedgerDir(file.parent_path())) {
    return false;
  }
  return true;
}

}  // namespace

std::filesystem::path defaultConfigRoot() {
  if (auto v = PJ::sdk::getEnv("MOSAICO_CONFIG_DIR"); v.has_value() && !v->empty()) {
    return fs::path(*v);
  }
  if (auto v = PJ::sdk::getEnv("XDG_CONFIG_HOME"); v.has_value() && !v->empty()) {
    return fs::path(*v) / "mosaico";
  }
#if defined(_WIN32)
  if (auto v = PJ::sdk::getEnv("APPDATA"); v.has_value() && !v->empty()) {
    return fs::path(*v) / "mosaico";
  }
#else
  if (auto v = PJ::sdk::getEnv("HOME"); v.has_value() && !v->empty()) {
    return fs::path(*v) / ".config" / "mosaico";
  }
#endif
  return {};
}

std::optional<std::string> trustedOriginKey(std::string_view uri) {
  const auto origin = parseGrpcOrigin(uri);
  if (!origin.has_value()) {
    return std::nullopt;
  }
  return serializeOrigin(*origin);
}

TrustedOrigins::TrustedOrigins(fs::path config_root) : path_(std::move(config_root)) {
  if (!path_.empty()) {
    path_ /= "trusted_origins.json";
  }
}

TrustedOrigins TrustedOrigins::standard() {
  return TrustedOrigins(defaultConfigRoot());
}

bool TrustedOrigins::recordSuccessfulConnect(std::string_view uri) {
  if (path_.empty()) {
    return false;  // no resolvable config root: nothing durable is possible
  }
  const auto origin = parseGrpcOrigin(uri);
  if (!origin.has_value()) {
    return false;  // unparsable uri: fail closed, record nothing
  }
  const std::string entry = serializeOrigin(*origin);
  // Serialize the WHOLE read-modify-write across processes: bounded retry on
  // the exclusive sidecar lock, then RE-read under it so a concurrent append
  // is merged, never overwritten.
  ensureDir0700(path_.parent_path());
  const fs::path lock_path = path_.parent_path() / (path_.filename().string() + ".lock");
  std::optional<FileLock> lock;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    lock = FileLock::tryExclusive(lock_path, nullptr);
    if (lock.has_value()) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;  // could not serialize — nothing durable happened
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  nlohmann::json origins = loadOrigins(path_);
  for (const auto& existing : origins) {
    if (existing.is_string() && existing.get<std::string>() == entry) {
      return true;  // already recorded — idempotent, durably present
    }
  }
  origins.push_back(entry);
  return writeOrigins(path_, origins);
}

std::vector<std::string> TrustedOrigins::allOrigins() const {
  std::vector<std::string> out;
  if (path_.empty()) {
    return out;
  }
  const nlohmann::json origins = loadOrigins(path_);
  out.reserve(origins.size());
  for (const auto& existing : origins) {
    if (existing.is_string()) {
      out.push_back(existing.get<std::string>());
    }
  }
  return out;
}

bool TrustedOrigins::isTrusted(std::string_view uri) const {
  const auto key = trustedOriginKey(uri);
  if (!key.has_value() || path_.empty()) {
    return false;  // rejected shapes never match, not even themselves
  }
  const nlohmann::json origins = loadOrigins(path_);
  for (const auto& existing : origins) {
    if (existing.is_string() && existing.get<std::string>() == *key) {
      return true;
    }
  }
  return false;
}

namespace testing {
void setTrustedOriginsDirSyncFailForTest(bool fail) {
  g_fail_dir_sync_for_test = fail;
}
}  // namespace testing

}  // namespace mosaico
