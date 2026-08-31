// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "promotion_artifact_lease_registry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

struct TempDir {
  TempDir() {
    path = fs::temp_directory_path() / ("mosaico-promotion-lease-test-" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code error;
    fs::remove_all(path, error);
  }
  fs::path path;
};

TEST(PromotionArtifactLeaseRegistry, SuccessPinsUntilRegistryDestructionAndHandlesReentrantResult) {
  TempDir temp;
  const fs::path artifact = temp.path / "artifact.pjmosaico";
  const fs::path lock_path = temp.path / "artifact.pjmosaico.lock";
  std::string error;
  int settlements = 0;

  {
    mosaico::PromotionArtifactLeaseRegistry registry;
    auto lease = mosaico::FileLock::tryShared(lock_path, &error);
    ASSERT_TRUE(lease.has_value()) << error;

    std::weak_ptr<mosaico::PendingPromotionArtifactLease> weak_pending;
    auto pending = std::make_shared<mosaico::PendingPromotionArtifactLease>(
        registry, artifact, std::move(*lease), [&weak_pending, &settlements](bool ok, std::string) {
          ++settlements;
          EXPECT_TRUE(ok);
          if (auto reentrant = weak_pending.lock()) {
            reentrant->settle(/*ok=*/false, "duplicate");
          }
        });
    weak_pending = pending;
    pending->settle(/*ok=*/true, "promoted");

    EXPECT_EQ(settlements, 1);
    EXPECT_EQ(registry.retainedCount(), 1u);
    EXPECT_TRUE(registry.retains(artifact));
    EXPECT_FALSE(mosaico::FileLock::tryExclusive(lock_path, &error).has_value());
  }

  EXPECT_TRUE(mosaico::FileLock::tryExclusive(lock_path, &error).has_value()) << error;
}

TEST(PromotionArtifactLeaseRegistry, FailureReleasesPendingLease) {
  TempDir temp;
  const fs::path artifact = temp.path / "artifact.pjmosaico";
  const fs::path lock_path = temp.path / "artifact.pjmosaico.lock";
  std::string error;
  mosaico::PromotionArtifactLeaseRegistry registry;
  auto lease = mosaico::FileLock::tryShared(lock_path, &error);
  ASSERT_TRUE(lease.has_value()) << error;

  bool settled = false;
  auto pending = std::make_shared<mosaico::PendingPromotionArtifactLease>(
      registry, artifact, std::move(*lease), [&settled](bool ok, std::string) {
        settled = true;
        EXPECT_FALSE(ok);
      });
  pending->settle(/*ok=*/false, "rejected");

  EXPECT_TRUE(settled);
  EXPECT_EQ(registry.retainedCount(), 0u);
  EXPECT_TRUE(mosaico::FileLock::tryExclusive(lock_path, &error).has_value()) << error;
}

}  // namespace
