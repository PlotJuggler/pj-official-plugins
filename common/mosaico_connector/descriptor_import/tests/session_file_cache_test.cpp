// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Session file cache core, fully hermetic: a private temp root is injected
// (never the real $XDG_CACHE_HOME), and artifact validation is a STUB — the
// suite proves the locking / partial / atomic-rename / lease / LRU mechanics
// independently of the artifact format. The real Arrow-IPC validator gets its
// own suite beside the artifact writer.
#include "descriptor_import/session_file_cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "descriptor_import/tests/test_support_fs.hpp"

namespace {

namespace fs = std::filesystem;
using mosaico::FileLock;
using mosaico::SessionFileCache;

struct TempRoot : mosaico_test::ScopedTempDir {
  explicit TempRoot(const std::string& name) : ScopedTempDir("mosaico-cache-test-" + name) {}
};

const std::string kValidHexA(32, 'a');
const std::string kValidHexB(32, 'b');
const std::string kValidHexC(32, 'c');

std::string identityFor(const std::string& hex) {
  return "mosaico:v1:sha256/128:" + hex;
}

SessionFileCache::Validator acceptAll() {
  return [](const fs::path&, const std::string&, std::string*) { return true; };
}

SessionFileCache::Validator rejectAll(const std::string& reason) {
  return [reason](const fs::path&, const std::string&, std::string* error) {
    if (error) {
      *error = reason;
    }
    return false;
  };
}

void writeFile(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

// Materialize `hex` through the real lock -> partial -> finalize path.
fs::path materialize(SessionFileCache& cache, const std::string& hex, const std::string& content) {
  const std::string identity = identityFor(hex);
  std::string error;
  auto lock = cache.tryLockForMaterialize(identity, &error);
  EXPECT_TRUE(lock.has_value()) << error;
  if (!lock.has_value()) {
    return {};
  }
  writeFile(cache.partialPathFor(*lock), content);
  EXPECT_TRUE(cache.finalize(*lock, &error)) << error;
  return cache.pathFor(identity);
}

}  // namespace

TEST(SessionFileCache, PathForShapeAndIdentityValidation) {
  TempRoot root("pathfor");
  SessionFileCache cache(root.path, acceptAll());
  EXPECT_EQ(cache.pathFor(identityFor(kValidHexA)), root.path / (kValidHexA + ".pjmosaico"));
  // Malformed identities can never name a file.
  EXPECT_TRUE(cache.pathFor("").empty());
  EXPECT_TRUE(cache.pathFor("mosaico:v1:sha256/128:short").empty());
  EXPECT_TRUE(cache.pathFor("mcap-cloud:v1:sha256/128:" + kValidHexA).empty());
  EXPECT_TRUE(cache.pathFor(identityFor(std::string(32, 'A'))).empty());  // uppercase
  EXPECT_TRUE(cache.pathFor(identityFor(std::string(31, 'a') + "g")).empty());
  EXPECT_TRUE(cache.pathFor(identityFor(kValidHexA) + "x").empty());  // trailing junk
}

TEST(SessionFileCache, MaterializeFinalizeLookupRoundTrip) {
  TempRoot root("roundtrip");
  SessionFileCache cache(root.path, acceptAll());
  const fs::path final_path = materialize(cache, kValidHexA, "artifact-bytes");
  ASSERT_FALSE(final_path.empty());
  EXPECT_TRUE(fs::is_regular_file(final_path));
  // The partial is gone; the LRU stamp exists.
  bool partial_left = false;
  for (const auto& entry : fs::directory_iterator(root.path)) {
    if (entry.path().filename().string().find(".partial.") != std::string::npos) {
      partial_left = true;
    }
  }
  EXPECT_FALSE(partial_left);
  EXPECT_TRUE(fs::is_regular_file(fs::path(final_path.string() + ".touch")));

  fs::path hit;
  EXPECT_TRUE(cache.lookup(identityFor(kValidHexA), &hit));
  EXPECT_EQ(hit, final_path);
  EXPECT_FALSE(cache.lookup(identityFor(kValidHexB), nullptr));  // absent = miss
}

TEST(SessionFileCache, ValidatorRunsForFinalizeAndLookup) {
  TempRoot root("validator-args");
  std::vector<std::string> seen;
  SessionFileCache cache(root.path, [&seen](const fs::path&, const std::string& hex, std::string*) {
    EXPECT_EQ(hex, kValidHexA);
    seen.push_back(hex);
    return true;
  });
  (void)materialize(cache, kValidHexA, "bytes");
  EXPECT_TRUE(cache.lookup(identityFor(kValidHexA), nullptr));
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], kValidHexA);
  EXPECT_EQ(seen[1], kValidHexA);
}

TEST(SessionFileCache, FinalizeRejectionDeletesPartialAndReportsReason) {
  TempRoot root("reject");
  SessionFileCache cache(root.path, rejectAll("stub says no"));
  std::string error;
  auto lock = cache.tryLockForMaterialize(identityFor(kValidHexA), &error);
  ASSERT_TRUE(lock.has_value()) << error;
  const fs::path partial = cache.partialPathFor(*lock);
  writeFile(partial, "junk");
  EXPECT_FALSE(cache.finalize(*lock, &error));
  EXPECT_NE(error.find("stub says no"), std::string::npos) << error;
  EXPECT_FALSE(fs::exists(partial));  // partials never survive
  EXPECT_FALSE(fs::exists(cache.pathFor(identityFor(kValidHexA))));
}

TEST(SessionFileCache, NullValidatorFailsClosed) {
  TempRoot root("null-validator");
  SessionFileCache cache(root.path, nullptr);
  // A pre-existing file at the right path must NOT classify as a hit.
  writeFile(root.path / (kValidHexA + ".pjmosaico"), "foreign");
  EXPECT_FALSE(cache.lookup(identityFor(kValidHexA), nullptr));
  std::string error;
  auto lock = cache.tryLockForMaterialize(identityFor(kValidHexB), &error);
  ASSERT_TRUE(lock.has_value()) << error;
  writeFile(cache.partialPathFor(*lock), "bytes");
  EXPECT_FALSE(cache.finalize(*lock, &error));
  EXPECT_NE(error.find("no artifact validator"), std::string::npos) << error;
}

TEST(SessionFileCache, LookupMissDoesNotDeleteTheFile) {
  TempRoot root("miss-keeps");
  SessionFileCache cache(root.path, rejectAll("invalid"));
  const fs::path file = root.path / (kValidHexA + ".pjmosaico");
  writeFile(file, "not-an-artifact");
  EXPECT_FALSE(cache.lookup(identityFor(kValidHexA), nullptr));
  EXPECT_TRUE(fs::exists(file));  // deletion policy is the provider flow's
}

TEST(SessionFileCache, SecondMaterializeLockOnSameIdentityFailsWhileHeld) {
  TempRoot root("contend");
  SessionFileCache cache(root.path, acceptAll());
  std::string error;
  auto first = cache.tryLockForMaterialize(identityFor(kValidHexA), &error);
  ASSERT_TRUE(first.has_value()) << error;
  bool contended = false;
  auto second = cache.tryLockForMaterialize(identityFor(kValidHexA), &error, &contended);
  EXPECT_FALSE(second.has_value());
  EXPECT_TRUE(contended);  // held elsewhere = retry-able
  // A different identity is unaffected.
  auto other = cache.tryLockForMaterialize(identityFor(kValidHexB), &error);
  EXPECT_TRUE(other.has_value()) << error;
  // Malformed identity: an error, never contention.
  contended = true;
  auto bad = cache.tryLockForMaterialize("junk", &error, &contended);
  EXPECT_FALSE(bad.has_value());
  EXPECT_FALSE(contended);
}

TEST(SessionFileCache, ToSharedLeaseBlocksMaterializeButAllowsLookup) {
  TempRoot root("lease-handoff");
  SessionFileCache cache(root.path, acceptAll());
  const std::string identity = identityFor(kValidHexA);
  std::string error;
  auto lock = cache.tryLockForMaterialize(identity, &error);
  ASSERT_TRUE(lock.has_value()) << error;
  writeFile(cache.partialPathFor(*lock), "bytes");
  ASSERT_TRUE(cache.finalize(*lock, &error)) << error;
  auto lease = SessionFileCache::toSharedLease(std::move(*lock), &error);
  ASSERT_TRUE(lease.has_value()) << error;
  // The lease blocks a re-materialization (contended)...
  bool contended = false;
  EXPECT_FALSE(cache.tryLockForMaterialize(identity, &error, &contended).has_value());
  EXPECT_TRUE(contended);
  // ...but lookup (no lock taken) still hits.
  EXPECT_TRUE(cache.lookup(identity, nullptr));
}

TEST(SessionFileCache, ReadLeaseRejectsMalformedIdentity) {
  TempRoot root("lease-id");
  SessionFileCache cache(root.path, acceptAll());
  std::string error;
  EXPECT_FALSE(cache.acquireReadLease("junk", &error).has_value());
  EXPECT_NE(error.find("invalid descriptor identity"), std::string::npos);
  EXPECT_TRUE(cache.acquireReadLease(identityFor(kValidHexA), &error).has_value()) << error;
}

TEST(SessionFileCache, FileLockSharedExclusiveContract) {
  TempRoot root("locks");
  const fs::path lock_path = root.path / "x.lock";
  std::string error;
  auto shared_a = FileLock::tryShared(lock_path, &error);
  ASSERT_TRUE(shared_a.has_value()) << error;
  auto shared_b = FileLock::tryShared(lock_path, &error);
  EXPECT_TRUE(shared_b.has_value()) << error;  // shared holders stack
  EXPECT_FALSE(FileLock::tryExclusive(lock_path, &error).has_value());
  shared_a.reset();
  shared_b.reset();
  auto exclusive = FileLock::tryExclusive(lock_path, &error);
  ASSERT_TRUE(exclusive.has_value()) << error;
  EXPECT_FALSE(FileLock::tryShared(lock_path, &error).has_value());
}

TEST(SessionFileCache, CleanupRemovesStaleOrphanPartialsOnly) {
  TempRoot root("orphans");
  SessionFileCache cache(root.path, acceptAll());
  const fs::path stale = root.path / (kValidHexA + ".pjmosaico.partial.99999");
  const fs::path fresh = root.path / (kValidHexB + ".pjmosaico.partial.99998");
  writeFile(stale, "stale");
  writeFile(fresh, "fresh");
  fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(48));

  // A partial whose identity lock is HELD stays even when old.
  const fs::path held = root.path / (kValidHexC + ".pjmosaico.partial.99997");
  writeFile(held, "held");
  fs::last_write_time(held, fs::file_time_type::clock::now() - std::chrono::hours(48));
  std::string error;
  auto lock = FileLock::tryExclusive(root.path / (kValidHexC + ".pjmosaico.lock"), &error);
  ASSERT_TRUE(lock.has_value()) << error;

  cache.cleanup(SessionFileCache::Config{});
  EXPECT_FALSE(fs::exists(stale));  // old + unlocked: collected
  EXPECT_TRUE(fs::exists(fresh));   // young: kept
  EXPECT_TRUE(fs::exists(held));    // locked: kept
}

TEST(SessionFileCache, CleanupEvictsOldestTouchedFirstAndStopsAtCap) {
  TempRoot root("lru");
  SessionFileCache cache(root.path, acceptAll());
  const fs::path a = materialize(cache, kValidHexA, std::string(1024, 'a'));
  const fs::path b = materialize(cache, kValidHexB, std::string(1024, 'b'));
  // Make A's stamp clearly older than B's.
  fs::last_write_time(fs::path(a.string() + ".touch"), fs::file_time_type::clock::now() - std::chrono::hours(10));

  SessionFileCache::Config cfg;
  cfg.max_total_bytes = 1536;  // both = 2048 > cap; one eviction suffices
  cfg.min_free_bytes = 0;
  cache.cleanup(cfg);
  EXPECT_FALSE(fs::exists(a));  // oldest-touched evicted
  EXPECT_TRUE(fs::exists(b));
  EXPECT_FALSE(fs::exists(fs::path(a.string() + ".touch")));
}

TEST(SessionFileCache, CleanupSkipsLeasedVictimAndEvictsAfterRelease) {
  TempRoot root("lease-skip");
  SessionFileCache cache(root.path, acceptAll());
  const fs::path a = materialize(cache, kValidHexA, std::string(1024, 'a'));
  fs::last_write_time(fs::path(a.string() + ".touch"), fs::file_time_type::clock::now() - std::chrono::hours(10));
  std::string error;
  auto lease = cache.acquireReadLease(identityFor(kValidHexA), &error);
  ASSERT_TRUE(lease.has_value()) << error;

  SessionFileCache::Config cfg;
  cfg.max_total_bytes = 0;  // everything is over budget
  cfg.min_free_bytes = 0;
  cache.cleanup(cfg);
  EXPECT_TRUE(fs::exists(a));  // leased: never evicted under a holder

  lease.reset();
  cache.cleanup(cfg);
  EXPECT_FALSE(fs::exists(a));  // lease gone: evictable
}

#if !defined(_WIN32)
TEST(SessionFileCache, StandardHonoursCacheDirOverride) {
  TempRoot root("standard");
  ASSERT_EQ(::setenv("MOSAICO_CACHE_DIR", root.path.string().c_str(), 1), 0);
  std::string error;
  SessionFileCache cache = SessionFileCache::standard(acceptAll(), &error);
  EXPECT_EQ(cache.pathFor(identityFor(kValidHexA)), root.path / (kValidHexA + ".pjmosaico"));
  ::unsetenv("MOSAICO_CACHE_DIR");
}
#endif
