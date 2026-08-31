// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <pj_base/sdk/descriptor_import/request_cache.hpp>
#include <string>
#include <unordered_map>
#include <vector>

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
  /// artifact path. Never drops the lease: a path whose key cannot be computed
  /// (or an allocation failure) parks it keyless, still held until DSO unload.
  void retain(const std::filesystem::path& artifact_path, PJ::sdk::descriptor_import::ReadLease lease) noexcept;

  [[nodiscard]] std::size_t retainedCount() const;
  [[nodiscard]] bool retains(const std::filesystem::path& artifact_path) const;

 private:
  [[nodiscard]] static std::string stableKey(const std::filesystem::path& artifact_path);

  mutable std::mutex mu_;
  std::unordered_map<std::string, PJ::sdk::descriptor_import::ReadLease> leases_;
  std::vector<PJ::sdk::descriptor_import::ReadLease> parked_;  ///< keyless holds (see retain)
};

/// The toolbox DSO's registry. Its function-local static lifetime is
/// independent of dialog/provider instances and ends only at DSO unload.
PromotionArtifactLeaseRegistry& promotionArtifactLeaseRegistry();

/// Owns a promotion's lease until the asynchronous host result settles. A
/// successful first result moves it into the registry before notifying the
/// caller; failure or synchronous rejection releases it. Re-entrant or
/// duplicate results are ignored after the first settlement. The host's
/// verdict passes through untouched — once the host reports a successful
/// promotion it has already replaced the dataset, so nothing here may
/// re-label it as a failure — and settle() never throws into the host's
/// callback.
class PendingPromotionArtifactLease {
 public:
  using SettlementHandler = std::function<void(bool, std::string)>;

  PendingPromotionArtifactLease(
      PromotionArtifactLeaseRegistry& registry, std::filesystem::path artifact_path,
      PJ::sdk::descriptor_import::ReadLease lease, SettlementHandler handler);

  void settle(bool ok, std::string detail) noexcept;

 private:
  PromotionArtifactLeaseRegistry& registry_;
  std::filesystem::path artifact_path_;
  std::optional<PJ::sdk::descriptor_import::ReadLease> lease_;
  SettlementHandler handler_;
  std::mutex mu_;
  bool settled_ = false;
};

}  // namespace mosaico
