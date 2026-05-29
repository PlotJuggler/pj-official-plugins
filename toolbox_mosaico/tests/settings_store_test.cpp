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
#include <vector>

namespace {

// Owns the backend + host adapter so the produced SettingsView stays valid for
// the test's lifetime (the host's scratch buffers must outlive every view use).
struct Fixture {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  mosaico::SettingsStore store() {
    return mosaico::SettingsStore(PJ::sdk::SettingsView{host.view()});
  }
};

}  // namespace

TEST(SettingsStore, StringRoundTrip) {
  Fixture fx;
  auto store = fx.store();
  store.setString("key", "value");
  EXPECT_EQ(store.getString("key"), "value");
}

TEST(SettingsStore, StringListRoundTrip) {
  Fixture fx;
  auto store = fx.store();
  const std::vector<std::string> values = {"alpha", "beta", "gamma"};
  store.setStringList("key", values);
  EXPECT_EQ(store.getStringList("key"), values);
}

TEST(SettingsStore, IntRoundTrip) {
  Fixture fx;
  auto store = fx.store();
  store.setInt("key", 42);
  EXPECT_EQ(store.getInt("key", 0), 42);
}

TEST(SettingsStore, BoolRoundTrip) {
  Fixture fx;
  auto store = fx.store();
  store.setBool("key", true);
  EXPECT_TRUE(store.getBool("key", false));
}

TEST(SettingsStore, MissingKeysReturnDefaults) {
  Fixture fx;
  auto store = fx.store();
  EXPECT_EQ(store.getString("missing"), "");
  EXPECT_EQ(store.getInt("missing", 17), 17);
  EXPECT_TRUE(store.getBool("missing", true));
}

TEST(SettingsStore, WriteThroughVisibleToSecondStore) {
  // Two adapters over the same backend see each other's writes.
  Fixture fx;
  fx.store().setString("key", "persisted");
  EXPECT_EQ(fx.store().getString("key"), "persisted");
}

// Gap #5: the dialog persists the last selection under these two keys
// (mosaico/selected_sequence + mosaico/selected_topics) and restores them on
// the next connect. This pins the persist/restore round-trip for those exact
// keys through the same store the dialog uses.
TEST(SettingsStore, SelectedSequenceAndTopicsRoundTrip) {
  Fixture fx;
  auto store = fx.store();

  store.setString("mosaico/selected_sequence", "seq_2024_03");
  const std::vector<std::string> topics = {"/imu", "/camera/image", "/gps/fix"};
  store.setStringList("mosaico/selected_topics", topics);

  // A fresh adapter over the same backend (mirrors a later session's
  // initFromSettings reading what persistState wrote).
  EXPECT_EQ(fx.store().getString("mosaico/selected_sequence"), "seq_2024_03");
  EXPECT_EQ(fx.store().getStringList("mosaico/selected_topics"), topics);
}

TEST(SettingsStore, SelectedSelectionDefaultsWhenAbsent) {
  // First-ever launch: no selection persisted yet -> empty sequence, empty
  // topic list (the dialog treats these as "nothing to restore").
  Fixture fx;
  auto store = fx.store();
  EXPECT_EQ(store.getString("mosaico/selected_sequence"), "");
  EXPECT_TRUE(store.getStringList("mosaico/selected_topics").empty());
}

TEST(SettingsStore, UnboundViewReturnsDefaults) {
  // The settings service is optional; a default-constructed (unbound) view reads
  // defaults and silently drops writes.
  mosaico::SettingsStore store(PJ::sdk::SettingsView{});
  store.setString("key", "ignored");
  EXPECT_EQ(store.getString("key", "fallback"), "fallback");
  EXPECT_TRUE(store.getStringList("missing").empty());
  EXPECT_EQ(store.getInt("missing", 99), 99);
  EXPECT_FALSE(store.getBool("missing", false));
}
