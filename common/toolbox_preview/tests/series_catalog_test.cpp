// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Wiring tests for SeriesCatalog: that the type filter actually reaches names(), and that
// the structure signature notices a field changing TYPE.
//
// These cannot use PJ::testing::ToolboxTestStore: it hardcodes every field's catalog type
// to FLOAT64 (toolbox_test_store.hpp, trampolineAcquireCatalogSnapshot) and has no typed
// addField overload, so it can express neither a string nor a bool field — precisely the
// two cases under test. Hence the hand-rolled host below, which implements only
// acquire_catalog_snapshot; series reads are left failing on purpose, since what is being
// asserted here is catalog classification, not decoding (that is series_decode_test).

#include <gtest/gtest.h>
#include <pj_base/plugin_data_api.h>

#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <toolbox_preview/series_catalog.hpp>
#include <vector>

namespace {

using toolbox_preview::SeriesCatalog;

/// Minimal toolbox host that answers a catalog snapshot and nothing else.
class FakeCatalogHost {
 public:
  struct Field {
    std::string name;
    PJ_primitive_type_t type;
  };

  void addTopic(std::string topic_name, std::vector<Field> fields) {
    topics_.push_back(Topic{std::move(topic_name), std::move(fields)});
  }

  /// Retype one field in place, leaving every name and count untouched — the exact shape
  /// that a signature blind to types would fail to notice.
  void retypeField(std::size_t topic_index, std::size_t field_index, PJ_primitive_type_t type) {
    topics_[topic_index].fields[field_index].type = type;
  }

  [[nodiscard]] PJ::sdk::ToolboxHostView view() {
    static const PJ_toolbox_host_vtable_t vtable = makeVtable();
    return PJ::sdk::ToolboxHostView(PJ_toolbox_host_t{.ctx = this, .vtable = &vtable});
  }

 private:
  struct Topic {
    std::string name;
    std::vector<Field> fields;
  };

  static PJ_toolbox_host_vtable_t makeVtable() {
    PJ_toolbox_host_vtable_t v{};
    v.abi_version = PJ_ABI_VERSION;
    v.struct_size = sizeof(PJ_toolbox_host_vtable_t);
    v.acquire_catalog_snapshot = &FakeCatalogHost::acquireCatalogSnapshot;
    // read_series_arrow deliberately left null: SeriesCatalog must tolerate an unreadable
    // series by listing it without caching samples.
    return v;
  }

  // Flatten the topic/field tree into the three parallel arrays the ABI snapshot exposes.
  // Kept as members so the spans handed out stay alive for the caller's use.
  void rebuild() {
    sources_.clear();
    topic_infos_.clear();
    field_infos_.clear();

    for (std::uint32_t t = 0; t < topics_.size(); ++t) {
      const Topic& topic = topics_[t];
      const auto first_field = static_cast<std::uint32_t>(field_infos_.size());
      for (std::uint32_t f = 0; f < topic.fields.size(); ++f) {
        PJ_field_info_t info{};
        info.handle = PJ_field_handle_t{.topic = PJ_topic_handle_t{.id = t}, .id = f};
        info.name = PJ_string_view_t{topic.fields[f].name.data(), topic.fields[f].name.size()};
        info.type = topic.fields[f].type;
        field_infos_.push_back(info);
      }
      PJ_topic_info_t ti{};
      ti.handle = PJ_topic_handle_t{.id = t};
      ti.source = PJ_data_source_handle_t{.id = 0};
      ti.name = PJ_string_view_t{topic.name.data(), topic.name.size()};
      ti.first_field = first_field;
      ti.field_count = static_cast<std::uint32_t>(topic.fields.size());
      topic_infos_.push_back(ti);
    }

    PJ_data_source_info_t ds{};
    ds.handle = PJ_data_source_handle_t{.id = 0};
    ds.name = PJ_string_view_t{kSourceName.data(), kSourceName.size()};
    ds.first_topic = 0;
    ds.topic_count = static_cast<std::uint32_t>(topic_infos_.size());
    sources_.push_back(ds);
  }

  static bool acquireCatalogSnapshot(void* ctx, PJ_catalog_snapshot_t* out, PJ_error_t* /*err*/) noexcept {
    auto* self = static_cast<FakeCatalogHost*>(ctx);
    self->rebuild();  // built on demand, so a mutation cannot forget to refresh it
    out->data_sources = self->sources_.data();
    out->data_source_count = self->sources_.size();
    out->topics = self->topic_infos_.data();
    out->topic_count = self->topic_infos_.size();
    out->fields = self->field_infos_.data();
    out->field_count = self->field_infos_.size();
    out->release_ctx = nullptr;
    out->release = [](void*) noexcept {};  // storage is owned by the fixture
    return true;
  }

  static inline const std::string kSourceName{"fake"};

  std::vector<Topic> topics_;
  std::vector<PJ_data_source_info_t> sources_;
  std::vector<PJ_topic_info_t> topic_infos_;
  std::vector<PJ_field_info_t> field_infos_;
};

// ---------------------------------------------------------------------------

TEST(SeriesCatalogFilter, OnlyPlottableFieldsAreOffered) {
  FakeCatalogHost host;
  host.addTopic(
      "sensor", {{"f64", PJ_PRIMITIVE_TYPE_FLOAT64},
                 {"i32", PJ_PRIMITIVE_TYPE_INT32},
                 {"u8", PJ_PRIMITIVE_TYPE_UINT8},
                 {"flag", PJ_PRIMITIVE_TYPE_BOOL},
                 {"label", PJ_PRIMITIVE_TYPE_STRING}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refresh(host.view()));

  const std::vector<std::string> expected{"sensor/f64", "sensor/i32", "sensor/u8", "sensor/flag"};
  EXPECT_EQ(catalog.names(), expected);
}

TEST(SeriesCatalogFilter, UnspecifiedIsRejectedToo) {
  FakeCatalogHost host;
  host.addTopic("t", {{"untyped", PJ_PRIMITIVE_TYPE_UNSPECIFIED}, {"good", PJ_PRIMITIVE_TYPE_FLOAT64}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refresh(host.view()));
  EXPECT_EQ(catalog.names(), (std::vector<std::string>{"t/good"}));
}

TEST(SeriesCatalogFilter, ListedButUnreadableSeriesHasNoSamples) {
  // "In names() but not in has()" must mean the host read failed — not that the type was
  // unsupported, which is the ambiguity the filter removed.
  FakeCatalogHost host;
  host.addTopic("t", {{"good", PJ_PRIMITIVE_TYPE_FLOAT64}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refresh(host.view()));
  EXPECT_EQ(catalog.names(), (std::vector<std::string>{"t/good"}));
  EXPECT_FALSE(catalog.has("t/good"));  // this fake never serves series data
}

// ---------------------------------------------------------------------------
// Structure signature
// ---------------------------------------------------------------------------

TEST(SeriesCatalogSignature, RetypingAFieldTriggersARefresh) {
  // Regression test for a signature built only from source ids and per-topic field
  // counts: reloading a different file over the same shape retypes a field without
  // moving any count, and the type filter would stay frozen on the old types.
  FakeCatalogHost host;
  host.addTopic("t", {{"maybe", PJ_PRIMITIVE_TYPE_STRING}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refreshStructureIfChanged(host.view()));
  EXPECT_TRUE(catalog.names().empty());  // string → filtered out

  host.retypeField(0, 0, PJ_PRIMITIVE_TYPE_FLOAT64);
  EXPECT_TRUE(catalog.refreshStructureIfChanged(host.view()));
  EXPECT_EQ(catalog.names(), (std::vector<std::string>{"t/maybe"}));
}

TEST(SeriesCatalogSignature, AnUnchangedCatalogIsStillACheapNoOp) {
  // The per-tick guard must keep working: adding fields to the signature must not make
  // every 20 Hz tick re-read the whole catalog.
  FakeCatalogHost host;
  host.addTopic("t", {{"a", PJ_PRIMITIVE_TYPE_FLOAT64}, {"b", PJ_PRIMITIVE_TYPE_INT32}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refreshStructureIfChanged(host.view()));
  EXPECT_FALSE(catalog.refreshStructureIfChanged(host.view()));
  EXPECT_FALSE(catalog.refreshStructureIfChanged(host.view()));
}

TEST(SeriesCatalogSignature, AddingATopicTriggersARefresh) {
  FakeCatalogHost host;
  host.addTopic("a", {{"x", PJ_PRIMITIVE_TYPE_FLOAT64}});

  SeriesCatalog catalog;
  ASSERT_TRUE(catalog.refreshStructureIfChanged(host.view()));
  host.addTopic("b", {{"y", PJ_PRIMITIVE_TYPE_FLOAT64}});
  EXPECT_TRUE(catalog.refreshStructureIfChanged(host.view()));
  EXPECT_EQ(catalog.names(), (std::vector<std::string>{"a/x", "b/y"}));
}

}  // namespace
