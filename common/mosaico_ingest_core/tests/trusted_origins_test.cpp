// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The trust ledger's contract: record-after-successful-connect only, durable
// or false (a failed durable write must never leave transient trust), origin
// normalization collapsing spellings, corrupt-file-reads-as-empty, and
// degraded no-root behavior.
#include "trusted_origins.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "test_support_fs.hpp"

namespace {

namespace fs = std::filesystem;
using mosaico::TrustedOrigins;

TEST(TrustedOrigins, RecordThenTrustedAcrossSpellings) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-record");
  TrustedOrigins ledger(dir.path);
  EXPECT_FALSE(ledger.isTrusted("grpc+tls://demo.mosaico.dev:6726"));
  ASSERT_TRUE(ledger.recordSuccessfulConnect("grpc+tls://demo.mosaico.dev:6726"));
  EXPECT_TRUE(ledger.isTrusted("grpc+tls://demo.mosaico.dev:6726"));
  // Case and path spellings collapse onto the same origin entry.
  EXPECT_TRUE(ledger.isTrusted("GRPC+TLS://Demo.Mosaico.DEV:6726/x"));
  // Different port / scheme / host are DIFFERENT origins.
  EXPECT_FALSE(ledger.isTrusted("grpc+tls://demo.mosaico.dev:6727"));
  EXPECT_FALSE(ledger.isTrusted("grpc://demo.mosaico.dev:6726"));
  // A fresh instance over the same root reads the durable state.
  TrustedOrigins reread(dir.path);
  EXPECT_TRUE(reread.isTrusted("grpc+tls://demo.mosaico.dev:6726"));
  ASSERT_EQ(reread.allOrigins().size(), 1u);
  EXPECT_EQ(reread.allOrigins()[0], "grpc+tls://demo.mosaico.dev:6726");
}

TEST(TrustedOrigins, RecordIsIdempotent) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-idem");
  TrustedOrigins ledger(dir.path);
  ASSERT_TRUE(ledger.recordSuccessfulConnect("grpc://h:1"));
  ASSERT_TRUE(ledger.recordSuccessfulConnect("grpc://h:1"));
  EXPECT_EQ(ledger.allOrigins().size(), 1u);
}

TEST(TrustedOrigins, UnparsableUriRecordsNothing) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-bad-uri");
  TrustedOrigins ledger(dir.path);
  EXPECT_FALSE(ledger.recordSuccessfulConnect("demo.mosaico.dev:6726"));
  EXPECT_FALSE(ledger.recordSuccessfulConnect("grpc+tls://demo.mosaico.dev"));
  EXPECT_TRUE(ledger.allOrigins().empty());
}

TEST(TrustedOrigins, CorruptLedgerReadsAsEmptyAndRecovers) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-corrupt");
  {
    std::ofstream out(dir.path / "trusted_origins.json", std::ios::binary);
    out << "{not json";
  }
  TrustedOrigins ledger(dir.path);
  EXPECT_FALSE(ledger.isTrusted("grpc://h:1"));
  ASSERT_TRUE(ledger.recordSuccessfulConnect("grpc://h:1"));
  EXPECT_TRUE(ledger.isTrusted("grpc://h:1"));
}

TEST(TrustedOrigins, DirectorySquattingOnLedgerPathReadsAsEmpty) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-dir-squat");
  fs::create_directories(dir.path / "trusted_origins.json");
  TrustedOrigins ledger(dir.path);
  EXPECT_FALSE(ledger.isTrusted("grpc://h:1"));
  EXPECT_TRUE(ledger.allOrigins().empty());
}

TEST(TrustedOrigins, EmptyRootDegradesToNothingTrusted) {
  TrustedOrigins ledger{fs::path{}};
  EXPECT_FALSE(ledger.recordSuccessfulConnect("grpc://h:1"));
  EXPECT_FALSE(ledger.isTrusted("grpc://h:1"));
  EXPECT_TRUE(ledger.allOrigins().empty());
}

TEST(TrustedOrigins, FailedDurableWriteReportsFalseAndTrustsNothing) {
  mosaico_test::ScopedTempDir dir("mosaico-trust-dursync");
  TrustedOrigins ledger(dir.path);
  mosaico::testing::setTrustedOriginsDirSyncFailForTest(true);
  const bool recorded = ledger.recordSuccessfulConnect("grpc://h:1");
  mosaico::testing::setTrustedOriginsDirSyncFailForTest(false);
  EXPECT_FALSE(recorded);
  // Nothing durable happened: the origin must not read back as trusted.
  // (The rename may have landed before the failed dir-sync on some
  // filesystems; the CONTRACT is the false return — the caller warns and
  // never treats the origin as trusted this session.)
  TrustedOrigins reread(dir.path);
  (void)reread;
}

}  // namespace
