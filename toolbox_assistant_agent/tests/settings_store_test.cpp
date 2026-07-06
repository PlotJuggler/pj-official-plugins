// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// SettingsStore is a thin adapter over the host's pj.settings.v1 service. These
// tests drive it through an in-memory SettingsBackend wired up exactly as a host
// would (SettingsStoreHost::view() -> SettingsView).
#include "settings_store.hpp"

#include <gtest/gtest.h>

#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

namespace {

// Owns the backend + host adapter so the produced SettingsView stays valid for
// the test's lifetime (the host's scratch buffers must outlive every view use).
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
  store.setString("assistant.ollama.url", "http://localhost:11434");
  EXPECT_EQ(store.getString("assistant.ollama.url"), "http://localhost:11434");
}

TEST(AssistantSettingsStore, BackendChoiceRoundTrip) {
  // The dialog persists the backend selection here and restores it on the next
  // launch (initFromSettings reads exactly this key).
  Fixture fx;
  fx.store().setString("assistant.backend", "claude");
  EXPECT_EQ(fx.store().getString("assistant.backend"), "claude");
}

TEST(AssistantSettingsStore, MissingBackendDefaultsEmpty) {
  // First-ever launch: no backend chosen yet -> empty, which the dialog treats
  // as "surface the echo stub / prompt for settings".
  Fixture fx;
  EXPECT_EQ(fx.store().getString("assistant.backend", ""), "");
}

TEST(AssistantSettingsStore, AllKeysPersistAcrossFreshStore) {
  Fixture fx;
  {
    auto store = fx.store();
    store.setString("assistant.backend", "ollama");
    store.setString("assistant.ollama.url", "http://box:11434");
    store.setString("assistant.ollama.model", "qwen2.5");
    store.setString("assistant.claude.model", "claude-opus-4-8");
    store.setString("assistant.claude.cli_path", "/usr/local/bin/claude");
  }
  // A fresh adapter over the same backend mirrors a later session reading back.
  auto store = fx.store();
  EXPECT_EQ(store.getString("assistant.backend"), "ollama");
  EXPECT_EQ(store.getString("assistant.ollama.url"), "http://box:11434");
  EXPECT_EQ(store.getString("assistant.ollama.model"), "qwen2.5");
  EXPECT_EQ(store.getString("assistant.claude.model"), "claude-opus-4-8");
  EXPECT_EQ(store.getString("assistant.claude.cli_path"), "/usr/local/bin/claude");
}

TEST(AssistantSettingsStore, UnboundViewReturnsDefaults) {
  // The settings service is optional; a default-constructed (unbound) view reads
  // defaults and silently drops writes.
  assistant_agent::SettingsStore store(PJ::sdk::SettingsView{});
  store.setString("assistant.backend", "ignored");
  EXPECT_EQ(store.getString("assistant.backend", "ollama"), "ollama");
}
