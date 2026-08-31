// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "promotion_artifact_lease_registry.hpp"

#include <system_error>
#include <utility>

namespace mosaico {

std::string PromotionArtifactLeaseRegistry::stableKey(const std::filesystem::path& artifact_path) {
  std::error_code error;
  std::filesystem::path stable = std::filesystem::weakly_canonical(artifact_path, error);
  if (error) {
    error.clear();
    stable = std::filesystem::absolute(artifact_path, error);
  }
  if (error) {
    stable = artifact_path;
  }
  return stable.lexically_normal().string();
}

bool PromotionArtifactLeaseRegistry::retain(
    const std::filesystem::path& artifact_path, PJ::sdk::descriptor_import::ReadLease lease) noexcept {
  try {
    const std::string key = stableKey(artifact_path);
    if (key.empty()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    leases_.insert_or_assign(key, std::move(lease));
    return true;
  } catch (...) {
    return false;
  }
}

std::size_t PromotionArtifactLeaseRegistry::retainedCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return leases_.size();
}

bool PromotionArtifactLeaseRegistry::retains(const std::filesystem::path& artifact_path) const {
  try {
    const std::string key = stableKey(artifact_path);
    std::lock_guard<std::mutex> lock(mu_);
    return leases_.find(key) != leases_.end();
  } catch (...) {
    return false;
  }
}

PromotionArtifactLeaseRegistry& promotionArtifactLeaseRegistry() {
  static PromotionArtifactLeaseRegistry registry;
  return registry;
}

PendingPromotionArtifactLease::PendingPromotionArtifactLease(
    PromotionArtifactLeaseRegistry& registry, std::filesystem::path artifact_path,
    PJ::sdk::descriptor_import::ReadLease lease, SettlementHandler handler)
    : registry_(registry),
      artifact_path_(std::move(artifact_path)),
      lease_(std::move(lease)),
      handler_(std::move(handler)) {}

void PendingPromotionArtifactLease::settle(bool ok, std::string detail) {
  std::optional<PJ::sdk::descriptor_import::ReadLease> lease;
  SettlementHandler handler;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (settled_) {
      return;
    }
    settled_ = true;
    if (ok && lease_.has_value()) {
      lease.emplace(std::move(*lease_));
    }
    lease_.reset();
    handler = std::move(handler_);
  }

  if (ok && (!lease.has_value() || !registry_.retain(artifact_path_, std::move(*lease)))) {
    ok = false;
    detail = "promotion succeeded but its artifact read lease could not be retained";
  }
  if (handler) {
    handler(ok, std::move(detail));
  }
}

}  // namespace mosaico
