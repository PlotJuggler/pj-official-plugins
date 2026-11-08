// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// catalogDigest: the listing of loaded data handed to the model at the top of
// every turn, against the SDK test store and the fake catalog hosts.
#include "catalog_digest.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <string>

#include "support/fake_multi_dataset_host.hpp"
#include "test_support/fake_catalog_host.hpp"
#include "test_support/fake_playback_viewport_hosts.hpp"

namespace {

using assistant_agent::catalogDigest;
using assistant_agent::testing::FakeMultiDatasetHost;
using toolbox_testing::FakeCatalogHost;
using toolbox_testing::FakePlaybackHost;

constexpr std::int64_t kSec = 1'000'000'000;

// Populate a small catalog: /imu with fields x (0..4) and y (constant 2).
// ToolboxTestStore is non-movable (it hands out pointers into itself), so
// callers construct it locally and pass it here by reference.
void populate(PJ::testing::ToolboxTestStore& store) {
  store.addTopic("/imu");
  store.addField("/imu", "x", {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec}, {0.0, 1.0, 2.0, 3.0, 4.0});
  store.addField("/imu", "y", {0, kSec, 2 * kSec}, {2.0, 2.0, 2.0});
}

// The digest is what replaces the discovery round-trips, so it has to carry the
// full paths...
TEST(CatalogDigest, ListsTopicsAndFields) {
  PJ::testing::ToolboxTestStore store;
  populate(store);
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()));
  EXPECT_NE(digest.find("/imu"), std::string::npos) << digest;
  EXPECT_NE(digest.find("x"), std::string::npos) << digest;
}

// ...and, when it cannot carry everything, say so loudly. A model that trusts a
// silently-truncated listing will tell the user a signal does not exist.
TEST(CatalogDigest, SaysSoWhenTruncated) {
  PJ::testing::ToolboxTestStore store;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    store.addTopic(name);
    store.addField(name, "value", {0}, {1.0});
  }
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()), /*budget_chars=*/1000);
  EXPECT_LT(digest.size(), 2000u) << "the digest must respect its budget";
  EXPECT_NE(digest.find("TRUNCATED"), std::string::npos) << digest;
  EXPECT_NE(digest.find("list_topics"), std::string::npos) << digest;
}

// The measured problem this tiering exists for: a catalog where a handful of
// topics are so field-heavy that giving every topic its full field list blows
// the budget for ALL of them — the old algorithm's only move — even though
// most topics are small enough to afford it easily. The mixed digest keeps
// every topic's full fields when cheap and only trims the expensive ones.
TEST(CatalogDigest, MixesFullPartialAndCounts) {
  FakeCatalogHost host;
  for (int i = 0; i < 30; ++i) {
    const std::string topic = "/small_" + (i < 10 ? std::string("0") : std::string()) + std::to_string(i);
    host.addTopic(topic);
    host.addField(topic, "s" + std::to_string(i) + "_alpha");
    host.addField(topic, "s" + std::to_string(i) + "_beta");
    host.addField(topic, "s" + std::to_string(i) + "_gamma");
  }
  for (int t = 0; t < 2; ++t) {
    const std::string topic = "/fat_" + std::to_string(t);
    host.addTopic(topic);
    for (int f = 0; f < 60; ++f) {
      std::string name = "channel_" + std::to_string(t) + "_";
      if (f < 10) {
        name += "0";
      }
      name += std::to_string(f);
      host.addField(topic, name);
    }
  }

  // Tight enough that the two 60-field topics cannot both afford a full
  // listing (each would cost ~800+ characters) while the 30 three-field
  // topics can (each costs only ~20) -- the budget the real ALFA catalog
  // exercises this same way, just smaller.
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/1800);

  EXPECT_EQ(digest.find("TRUNCATED"), std::string::npos) << "the mixed tiering must never need to drop a topic "
                                                            "entirely -- every topic gets at least a count: "
                                                         << digest;
  // Every small topic keeps its full field list.
  for (int i = 0; i < 30; ++i) {
    const std::string field = "s" + std::to_string(i) + "_gamma";
    EXPECT_NE(digest.find(field), std::string::npos) << "small topic " << i << " lost its fields: " << digest;
  }
  // Neither fat topic's full field list survives -- its last field is never
  // reached by a count or a 120-char partial list.
  EXPECT_EQ(digest.find("channel_0_59"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("channel_1_59"), std::string::npos) << digest;
  EXPECT_NE(digest.find("describe_topic gives the rest"), std::string::npos) << digest;
}

// A field type shared by 80%+ of the catalog is stated once and left off
// every field that has it; a minority type still gets its own annotation, so
// the reader can tell the exception from the rule.
TEST(CatalogDigest, OmitsTheDominantType) {
  FakeCatalogHost host;
  host.addTopic("/mostly_int");
  for (int i = 0; i < 9; ++i) {
    host.addField("/mostly_int", "n" + std::to_string(i), PJ_PRIMITIVE_TYPE_INT32);
  }
  host.addField("/mostly_int", "odd_one_out", PJ_PRIMITIVE_TYPE_FLOAT64);

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_NE(digest.find("fields are int32 unless marked"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("n0 (int32)"), std::string::npos) << "the dominant type must be left off: " << digest;
  EXPECT_NE(digest.find("n0"), std::string::npos) << digest;
  EXPECT_NE(digest.find("odd_one_out (float64)"), std::string::npos)
      << "the minority type must still be stated: " << digest;
}

// When the budget cannot afford full field lists for every topic that would
// like one, the cheapest upgrades go first -- and among ties, catalog order
// decides, not map/hash order, so a re-run never reshuffles who won.
TEST(CatalogDigest, PromotesCheapestFirstDeterministically) {
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "roll");
  host.addField("/imu", "pitch");
  host.addField("/imu", "yaw");
  // A much larger topic so the budget is tight enough that not everything
  // upgrades to full, without being so tight that /imu itself is squeezed.
  host.addTopic("/big");
  for (int f = 0; f < 40; ++f) {
    std::string name = "reading_";
    if (f < 10) {
      name += "0";
    }
    name += std::to_string(f);
    host.addField("/big", name);
  }

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/400);

  // /imu is small and comes first in the catalog: its full, tied-cost fields
  // win the budget deterministically over /big's.
  EXPECT_NE(digest.find("roll"), std::string::npos) << digest;
  EXPECT_NE(digest.find("pitch"), std::string::npos) << digest;
  EXPECT_NE(digest.find("yaw"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("reading_039"), std::string::npos) << "/big must not have won a full listing: " << digest;
}

// The floor beneath the mixed tiering: a budget so tight that not even a bare
// field COUNT fits for every topic falls back to the pre-existing plain
// truncation (full tree, then names only), unchanged.
TEST(CatalogDigest, FallsBackToPlainTruncationWhenNotEvenCountsFit) {
  FakeCatalogHost host;
  for (int i = 0; i < 400; ++i) {
    const std::string name = "/topic_with_a_fairly_long_name_" + std::to_string(i);
    host.addTopic(name);
    host.addField(name, "value");
  }

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()), /*budget_chars=*/1000);

  EXPECT_LT(digest.size(), 2000u) << "the digest must respect its budget";
  EXPECT_NE(digest.find("TRUNCATED"), std::string::npos) << digest;
  EXPECT_NE(digest.find("list_topics"), std::string::npos) << digest;
  EXPECT_EQ(digest.find("describe_topic gives the rest"), std::string::npos)
      << "this is the legacy truncation footer, not the mixed-tier one: " << digest;
}

// Object topics get one line each under their dataset, alongside the scalar
// topics -- so a model reading the digest at the top of a turn already knows
// a point cloud is loaded, without an extra list_topics round trip.
TEST(CatalogDigest, MentionsObjects) {
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "x");
  host.addObjectTopic("/cloud", "kPointCloud", 7, 0, 2 * kSec);

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_NE(digest.find("/imu"), std::string::npos) << digest;
  EXPECT_NE(digest.find("/cloud"), std::string::npos) << digest;
  EXPECT_NE(digest.find("object kPointCloud"), std::string::npos) << digest;
  EXPECT_NE(digest.find("7 entries"), std::string::npos) << digest;
}

TEST(CatalogDigest, SaysWhenNothingIsLoaded) {
  PJ::testing::ToolboxTestStore store;
  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(store.makeHost()));
  EXPECT_NE(digest.find("nothing is loaded"), std::string::npos) << digest;
}

// --- several datasets loaded at once ---------------------------------------

// With two runs loaded, a flat topic list leaves the model unable to tell them
// apart — so it can neither offer a comparison nor avoid mixing them.
TEST(CatalogDigest, GroupsTopicsByDatasetWhenSeveralAreLoaded) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_NE(digest.find("run_monday.mcap"), std::string::npos) << digest;
  EXPECT_NE(digest.find("run_friday.mcap"), std::string::npos) << digest;
  // The same topic name appears under both, which is the whole point: identical
  // names across runs are the norm, not a collision.
  EXPECT_LT(digest.find("run_monday.mcap"), digest.find("run_friday.mcap"));
  // The qualifier syntax is taught HERE, by the listing, not by the tool
  // schema: this line is paid for only in sessions that actually hold several
  // datasets, instead of in the prefix of every session.
  EXPECT_NE(digest.find("<dataset>:<topic>/<field>"), std::string::npos) << digest;
}

// --- addressing a series when several datasets are loaded -------------------

// The host's own display convention, "dataset:topic/field", addresses one

// One dataset is the overwhelmingly common case and naming it every time buys
// nothing but tokens — on every API call, several times a turn.
TEST(CatalogDigest, SaysNothingAboutDatasetsWhenThereIsOnlyOne) {
  FakeMultiDatasetHost host;
  host.addDataset("only.mcap", {"/imu", "/speed"});

  const std::string digest = catalogDigest(PJ::sdk::ToolboxHostView(host.makeHost()));

  EXPECT_EQ(digest.find("dataset"), std::string::npos) << digest;
  EXPECT_NE(digest.find("/imu"), std::string::npos) << digest;
}

// An object topic's range is in display seconds when the playback view can
// convert it, and in raw seconds, marked as such, when it cannot.
TEST(CatalogDigest, ObjectRangeUsesTheDisplayAxisAndMarksRawFallback) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 1000 * kSec, 1001 * kSec);
  FakePlaybackHost playback;
  playback.display_offset_ns = 1000 * kSec;
  const PJ::sdk::ToolboxHostView view(host.makeHost());
  EXPECT_NE(catalogDigest(view, 10000, playback.view()).find("0.000-1.000 display seconds"), std::string::npos);
  EXPECT_NE(catalogDigest(view).find("1000.000-1001.000 raw seconds"), std::string::npos);
}

}  // namespace
