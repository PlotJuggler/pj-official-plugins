// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The cache budget seam: cache_max_gb -> SDK CleanupPolicy, and the
// maintenance pass evicting only unleased artifacts once over budget.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <pj_base/sdk/descriptor_import/request_cache.hpp>
#include <string>
#include <utility>

#include "descriptor_import/arrow_cache_artifact.hpp"
#include "descriptor_import/source_descriptor.hpp"
#include "descriptor_import/tests/test_support_fs.hpp"

namespace {

namespace fs = std::filesystem;
using PJ::sdk::descriptor_import::CacheSpec;
using PJ::sdk::descriptor_import::CleanupPolicy;
using PJ::sdk::descriptor_import::RequestArtifactCache;

constexpr std::uintmax_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr std::uintmax_t kUnlimited = std::numeric_limits<std::uintmax_t>::max();

TEST(CacheMaintenance, PolicyFromGigabytes) {
  EXPECT_EQ(mosaico::cacheCleanupPolicy(20.0).max_total_bytes, 20 * kGiB);
  EXPECT_EQ(mosaico::cacheCleanupPolicy(0.5).max_total_bytes, kGiB / 2);
  EXPECT_EQ(mosaico::cacheCleanupPolicy(0.0).max_total_bytes, kUnlimited);   // unlimited
  EXPECT_EQ(mosaico::cacheCleanupPolicy(-1.0).max_total_bytes, kUnlimited);  // unlimited
  EXPECT_EQ(mosaico::cacheCleanupPolicy(1e30).max_total_bytes, kUnlimited);  // saturates
}

// A hermetic cache with an accept-all validator: this is about the SDK's
// eviction rules under Mosaico's policy, not the artifact format.
RequestArtifactCache::Hit publish(RequestArtifactCache& cache, const std::string& identity, std::size_t bytes) {
  auto txn = cache.beginWrite(identity);
  EXPECT_TRUE(txn) << txn.error().message;
  {
    std::ofstream out(txn->partialPath(), std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
  }
  auto hit = txn->commit();
  EXPECT_TRUE(hit) << hit.error().message;
  return std::move(*hit);
}

TEST(CacheMaintenance, EvictsOnlyUnleasedArtifactsOverBudget) {
  mosaico_test::ScopedTempDir root("mosaico-cache-maintenance");
  const auto& scheme = mosaico::sourceDescriptorPolicy().identity;
  RequestArtifactCache cache(
      CacheSpec{root.path, ".pjmosaico", scheme},
      [](const fs::path&, const std::string&, std::string*) { return true; });
  auto leased = publish(cache, scheme.identityFor("kept"), 1024);
  auto evictable = publish(cache, scheme.identityFor("evicted"), 1024);
  evictable.lease.release();

  // Under budget: nothing happens.
  auto result = mosaico::maintainCache(cache, mosaico::cacheCleanupPolicy(1.0));
  EXPECT_TRUE(result.target_met);
  EXPECT_EQ(result.bytes_reclaimed, 0u);
  EXPECT_TRUE(fs::exists(evictable.path));

  // Over budget: the unleased artifact goes, the leased (older!) one stays.
  CleanupPolicy tight;
  tight.max_total_bytes = 1536;
  result = mosaico::maintainCache(cache, tight);
  EXPECT_TRUE(result.target_met);
  EXPECT_EQ(result.bytes_reclaimed, 1024u);
  EXPECT_FALSE(fs::exists(evictable.path));
  EXPECT_TRUE(fs::exists(leased.path));
}

}  // namespace
