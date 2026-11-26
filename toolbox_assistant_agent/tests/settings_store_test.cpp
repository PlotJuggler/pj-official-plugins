// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives SettingsStore through an in-memory SettingsBackend wired up as a host would
// (SettingsStoreHost::view() -> SettingsView).
#include "settings_store.hpp"

#include <gtest/gtest.h>

#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

namespace {

// Owns the backend + host adapter: the host's scratch buffers must outlive every view use.
struct Fixture {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  assistant_agent::SettingsStore store() {
    return assistant_agent::SettingsStore(PJ::sdk::SettingsView{host.view()});
  }
};

}  // namespace

TEST(AssistantSettingsStore, StringRoundTrip) {
  Fixture fx;
  auto store = fx.store();
  store.setString("assistant.claude.cli_path", "/opt/claude/bin/claude");
  EXPECT_EQ(store.getString("assistant.claude.cli_path"), "/opt/claude/bin/claude");
}

TEST(AssistantSettingsStore, BackendChoiceRoundTrip) {
  Fixture fx;
  fx.store().setString("assistant.backend", "claude");
  EXPECT_EQ(fx.store().getString("assistant.backend"), "claude");
}

TEST(AssistantSettingsStore, MissingBackendDefaultsEmpty) {
  // An absent key returns the caller's default verbatim.
  Fixture fx;
  EXPECT_EQ(fx.store().getString("assistant.backend", ""), "");
}

TEST(AssistantSettingsStore, AllKeysPersistAcrossFreshStore) {
  Fixture fx;
  {
    auto store = fx.store();
    store.setString("assistant.backend", "claude");
    store.setString("assistant.claude.model", "claude-opus-4-8");
    store.setString("assistant.claude.cli_path", "/usr/local/bin/claude");
  }
  // A fresh adapter over the same backend mirrors a later session.
  auto store = fx.store();
  EXPECT_EQ(store.getString("assistant.backend"), "claude");
  EXPECT_EQ(store.getString("assistant.claude.model"), "claude-opus-4-8");
  EXPECT_EQ(store.getString("assistant.claude.cli_path"), "/usr/local/bin/claude");
}

TEST(AssistantSettingsStore, UnboundViewReturnsDefaults) {
  // The service is optional: an unbound view reads defaults and drops writes.
  assistant_agent::SettingsStore store(PJ::sdk::SettingsView{});
  store.setString("assistant.backend", "ignored");
  EXPECT_EQ(store.getString("assistant.backend", "claude"), "claude");
}
