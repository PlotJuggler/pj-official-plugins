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
using PJ::sdk::descriptor_import::CacheSpec;
using PJ::sdk::descriptor_import::IdentityScheme;
using PJ::sdk::descriptor_import::RequestArtifactCache;

const std::string kIdentity = "test:v1:sha256/128:" + std::string(32, 'a');

RequestArtifactCache cacheFor(const fs::path& root) {
  return RequestArtifactCache(
      CacheSpec{root, ".pjmosaico", IdentityScheme{"test:v1:sha256/128:", 32}},
      [](const fs::path&, const std::string&, std::string*) { return true; });
}

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
  auto cache = cacheFor(temp.path);
  const fs::path artifact = cache.pathFor(kIdentity);
  int settlements = 0;

  {
    mosaico::PromotionArtifactLeaseRegistry registry;
    auto lease = cache.acquireReadLease(kIdentity);
    ASSERT_TRUE(lease) << lease.error().message;

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
    auto blocked = cache.beginWrite(kIdentity);
    ASSERT_FALSE(blocked);
    EXPECT_TRUE(blocked.error().retryable);
  }

  auto writer = cache.beginWrite(kIdentity);
  EXPECT_TRUE(writer) << writer.error().message;
}

TEST(PromotionArtifactLeaseRegistry, FailureReleasesPendingLease) {
  TempDir temp;
  auto cache = cacheFor(temp.path);
  const fs::path artifact = cache.pathFor(kIdentity);
  mosaico::PromotionArtifactLeaseRegistry registry;
  auto lease = cache.acquireReadLease(kIdentity);
  ASSERT_TRUE(lease) << lease.error().message;

  bool settled = false;
  auto pending = std::make_shared<mosaico::PendingPromotionArtifactLease>(
      registry, artifact, std::move(*lease), [&settled](bool ok, std::string) {
        settled = true;
        EXPECT_FALSE(ok);
      });
  pending->settle(/*ok=*/false, "rejected");

  EXPECT_TRUE(settled);
  EXPECT_EQ(registry.retainedCount(), 0u);
  auto writer = cache.beginWrite(kIdentity);
  EXPECT_TRUE(writer) << writer.error().message;
}

}  // namespace
