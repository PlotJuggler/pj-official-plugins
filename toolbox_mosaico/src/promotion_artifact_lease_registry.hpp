// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "descriptor_import/core/file_lock.h"

namespace mosaico {

/// Process/DSO-lifetime approximation of promoted-dataset ownership. The
/// promotion ABI has no dataset-close callback, so one shared lease per stable
/// artifact path remains pinned until this registry is destroyed at DSO unload.
class PromotionArtifactLeaseRegistry {
 public:
  PromotionArtifactLeaseRegistry() = default;
  PromotionArtifactLeaseRegistry(const PromotionArtifactLeaseRegistry&) = delete;
  PromotionArtifactLeaseRegistry& operator=(const PromotionArtifactLeaseRegistry&) = delete;

  /// Retain `lease`, replacing any lease already held for the same normalized
  /// artifact path. Returns false only when the path/map could not be stored.
  [[nodiscard]] bool retain(const std::filesystem::path& artifact_path, FileLock lease) noexcept;

  [[nodiscard]] std::size_t retainedCount() const;
  [[nodiscard]] bool retains(const std::filesystem::path& artifact_path) const;

 private:
  [[nodiscard]] static std::string stableKey(const std::filesystem::path& artifact_path);

  mutable std::mutex mu_;
  std::unordered_map<std::string, FileLock> leases_;
};

/// The toolbox DSO's registry. Its function-local static lifetime is
/// independent of dialog/provider instances and ends only at DSO unload.
PromotionArtifactLeaseRegistry& promotionArtifactLeaseRegistry();

/// Owns a promotion's lease until the asynchronous host result settles. A
/// successful first result moves it into the registry before notifying the
/// caller; failure or synchronous rejection releases it. Re-entrant or
/// duplicate results are ignored after the first settlement.
class PendingPromotionArtifactLease {
 public:
  using SettlementHandler = std::function<void(bool, std::string)>;

  PendingPromotionArtifactLease(
      PromotionArtifactLeaseRegistry& registry, std::filesystem::path artifact_path, FileLock lease,
      SettlementHandler handler);

  void settle(bool ok, std::string detail);

 private:
  PromotionArtifactLeaseRegistry& registry_;
  std::filesystem::path artifact_path_;
  std::optional<FileLock> lease_;
  SettlementHandler handler_;
  std::mutex mu_;
  bool settled_ = false;
};

}  // namespace mosaico
