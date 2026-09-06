// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The headless credential origin guard: MOSAICO_API_KEY is released to a
// descriptor target ONLY when MOSAICO_URL is set and origin-matches it. Any
// widening here silently hands the env bearer token to whatever server a
// hostile layout names, so every rejected shape is pinned. Hermetic via
// setenv (POSIX-only, like the other env-driven tests).
#include "credential_resolve.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <pj_base/sdk/settings_store_host.hpp>

#if !defined(_WIN32)

namespace {

using mosaico::envKeyAllowedForTarget;
using mosaico::headlessTargetUri;
using mosaico::resolveCredentials;
using mosaico::resolveHeadlessCredentials;
using mosaico::saveCredentialsForUri;
using mosaico::ServerCredentials;

constexpr const char* kTarget = "grpc+tls://demo.mosaico.dev:6726";

struct EnvGuard {
  EnvGuard(const char* url, const char* key) {
    if (url != nullptr) {
      ::setenv("MOSAICO_URL", url, 1);
    } else {
      ::unsetenv("MOSAICO_URL");
    }
    if (key != nullptr) {
      ::setenv("MOSAICO_API_KEY", key, 1);
    } else {
      ::unsetenv("MOSAICO_API_KEY");
    }
  }
  ~EnvGuard() {
    ::unsetenv("MOSAICO_URL");
    ::unsetenv("MOSAICO_API_KEY");
  }
};

struct SettingsFixture {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  PJ::sdk::SettingsView view() {
    return PJ::sdk::SettingsView{host.view()};
  }
};

TEST(EnvKeyOriginGuard, Matrix) {
  // Released: exact origin, and spelling variants that normalize to it.
  EXPECT_TRUE(envKeyAllowedForTarget(kTarget, "grpc+tls://demo.mosaico.dev:6726"));
  EXPECT_TRUE(envKeyAllowedForTarget(kTarget, "GRPC+TLS://Demo.Mosaico.DEV:6726/path"));
  // Withheld: no binding, different origin components, unparsable shapes.
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, ""));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc+tls://demo.mosaico.dev:6727"));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc+tls://evil.example.com:6726"));
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "grpc://demo.mosaico.dev:6726"));  // scheme downgrade
  EXPECT_FALSE(envKeyAllowedForTarget(kTarget, "demo.mosaico.dev:6726"));
  EXPECT_FALSE(envKeyAllowedForTarget("not-a-uri", "grpc+tls://demo.mosaico.dev:6726"));
}

TEST(HeadlessCredentials, EnvKeyRequiresOriginBoundUrl) {
  // Unbound settings view: no stored per-server key, so the env tier decides.
  PJ::sdk::SettingsView view{};
  {
    EnvGuard env(nullptr, "msco_secret");
    EXPECT_TRUE(resolveHeadlessCredentials(view, kTarget).api_key.empty());
  }
  {
    EnvGuard env("grpc+tls://evil.example.com:6726", "msco_secret");
    EXPECT_TRUE(resolveHeadlessCredentials(view, kTarget).api_key.empty());
  }
  {
    EnvGuard env(kTarget, "msco_secret");
    EXPECT_EQ(resolveHeadlessCredentials(view, kTarget).api_key, "msco_secret");
  }
}

TEST(HeadlessCredentials, StoredTlsKeyRequiresExplicitOptInForPlaintextTarget) {
  EnvGuard env(nullptr, nullptr);
  SettingsFixture settings;
  ServerCredentials stored;
  stored.api_key = "stored_secret";

  // grpc and grpc+tls intentionally share a history/settings key. Without
  // the stored entry's explicit plaintext opt-in, that collision must not
  // downgrade its bearer token.
  saveCredentialsForUri(settings.view(), kTarget, stored);
  const std::string plaintext_target = "grpc://demo.mosaico.dev:6726";
  EXPECT_TRUE(resolveHeadlessCredentials(settings.view(), plaintext_target).api_key.empty());

  stored.allow_insecure = true;
  saveCredentialsForUri(settings.view(), kTarget, stored);
  EXPECT_EQ(resolveHeadlessCredentials(settings.view(), plaintext_target).api_key, "stored_secret");
}

TEST(HeadlessTargetUri, PlaintextOnlyByStoredOptIn) {
  EnvGuard env(nullptr, nullptr);
  const std::string origin = "demo.mosaico.dev:6726";
  {
    // No stored entry (and even an unbound view): TLS by default.
    PJ::sdk::SettingsView unbound{};
    EXPECT_EQ(headlessTargetUri(unbound, origin), "grpc+tls://demo.mosaico.dev:6726");
  }
  SettingsFixture settings;
  EXPECT_EQ(headlessTargetUri(settings.view(), origin), "grpc+tls://demo.mosaico.dev:6726");
  ServerCredentials stored;
  stored.allow_insecure = true;
  saveCredentialsForUri(settings.view(), origin, stored);
  EXPECT_EQ(headlessTargetUri(settings.view(), origin), "grpc://demo.mosaico.dev:6726");
}

TEST(InteractiveCredentials, EnvFallbackStaysUnguarded) {
  // The panel's resolve keeps the unguarded env fallback: the user chose the
  // server there, so the key applies to whatever they typed (PJ3 parity).
  PJ::sdk::SettingsView view{};
  EnvGuard env(nullptr, "msco_secret");
  EXPECT_EQ(resolveCredentials(view, kTarget).api_key, "msco_secret");
}

}  // namespace

#endif  // !_WIN32
