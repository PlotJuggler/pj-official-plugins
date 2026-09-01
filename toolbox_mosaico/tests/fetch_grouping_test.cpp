// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Locks in the multi-topic grouping contract for a Mosaico Download.
//
// The bug this guards against: when a sequence with several topics was pulled,
// the ingest helpers created a fresh DataSourceHandle PER TOPIC and registered
// each topic under the qualified "<sequence>/<topic>" name. Because
// DataEngine::createDataset never dedupes by name, every topic landed in a
// SEPARATE dataset that merely shared the sequence string — CatalogModel then
// disambiguated them as `seq`, `seq (2)`, `seq (3)`… and the topic name was
// doubly prefixed.
//
// The fix:
//   1. Create the DataSourceHandle ONCE per Download and reuse it for every
//      topic (datasetForFetch, mutex-guarded for the parallel pull callbacks).
//   2. Register topics with the BARE "<topic>" name (the dataset already
//      carries the sequence name).
//
// This test exercises the lower-level contract directly (no Flight/gRPC): a
// datasetForFetch-shaped helper creates the source exactly once even under
// concurrent callbacks, and every caller gets that one source.

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../src/arrow_ingest.hpp"

namespace {

// ---------------------------------------------------------------------------
// Recording fake host
//
// Backs a PJ_toolbox_host_t with a vtable that records every call we care
// about. Handles are minted as monotonically increasing ids so the test can
// assert that two topics share the SAME DataSourceHandle.
// ---------------------------------------------------------------------------
struct RecordedDataSource {
  std::string name;
  std::uint32_t id = 0;
};
struct RecordedTopic {
  std::uint32_t source_id = 0;
  std::string name;
  std::uint32_t id = 0;
};
struct RecordedObjectTopic {
  std::uint32_t source_id = 0;
  std::string name;
  std::string metadata_json;
  std::uint32_t id = 0;
};

struct FakeHost {
  std::vector<RecordedDataSource> data_sources;
  std::vector<RecordedTopic> topics;
  std::vector<RecordedObjectTopic> object_topics;
  std::uint32_t next_id = 1;
  std::mutex mu;

  PJ::sdk::ToolboxHostView view() {
    PJ_toolbox_host_t host{};
    host.ctx = this;
    host.vtable = &kVtable;
    return PJ::sdk::ToolboxHostView(host);
  }

  static FakeHost* self(void* ctx) {
    return static_cast<FakeHost*>(ctx);
  }
  static std::string toStr(PJ_string_view_t s) {
    return (s.data != nullptr && s.size > 0) ? std::string(s.data, s.size) : std::string();
  }

  static bool createDataSource(void* ctx, PJ_string_view_t name, PJ_data_source_handle_t* out_source, PJ_error_t*)
      PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    const std::uint32_t id = h->next_id++;
    h->data_sources.push_back({toStr(name), id});
    out_source->id = id;
    return true;
  }
  static bool ensureTopic(
      void* ctx, PJ_data_source_handle_t source, PJ_string_view_t topic_name, PJ_topic_handle_t* out_topic,
      PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    const std::uint32_t id = h->next_id++;
    h->topics.push_back({source.id, toStr(topic_name), id});
    out_topic->id = id;
    return true;
  }
  static bool ensureField(
      void*, PJ_topic_handle_t, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t*, PJ_error_t*) PJ_NOEXCEPT {
    return false;
  }
  static bool appendRecord(void*, PJ_topic_handle_t, int64_t, const PJ_named_field_value_t*, uint64_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool appendBoundRecord(void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool acquireCatalogSnapshot(void*, PJ_catalog_snapshot_t*, PJ_error_t*) PJ_NOEXCEPT {
    return false;
  }
  static bool readSeriesArrow(void*, PJ_field_handle_t, struct ArrowSchema*, struct ArrowArray*, PJ_error_t*)
      PJ_NOEXCEPT {
    return false;
  }
  static bool registerObjectTopic(
      void* ctx, PJ_data_source_handle_t source, PJ_string_view_t topic_name, PJ_string_view_t metadata_json,
      PJ_object_topic_handle_t* out_handle, PJ_error_t*) PJ_NOEXCEPT {
    auto* h = self(ctx);
    std::lock_guard<std::mutex> lk(h->mu);
    const std::uint32_t id = h->next_id++;
    h->object_topics.push_back({source.id, toStr(topic_name), toStr(metadata_json), id});
    out_handle->id = id;
    return true;
  }
  static bool pushOwnedObject(void*, PJ_object_topic_handle_t, int64_t, const uint8_t*, uint64_t, PJ_error_t*)
      PJ_NOEXCEPT {
    return true;
  }

  static const PJ_toolbox_host_vtable_t kVtable;
};

const PJ_toolbox_host_vtable_t FakeHost::kVtable = [] {
  PJ_toolbox_host_vtable_t v{};
  v.abi_version = 0;
  v.struct_size = sizeof(PJ_toolbox_host_vtable_t);
  v.create_data_source = &FakeHost::createDataSource;
  v.ensure_topic = &FakeHost::ensureTopic;
  v.ensure_field = &FakeHost::ensureField;
  v.append_record = &FakeHost::appendRecord;
  v.append_bound_record = &FakeHost::appendBoundRecord;
  v.acquire_catalog_snapshot = &FakeHost::acquireCatalogSnapshot;
  v.read_series_arrow = &FakeHost::readSeriesArrow;
  v.register_object_topic = &FakeHost::registerObjectTopic;
  v.push_owned_object = &FakeHost::pushOwnedObject;
  return v;
}();

// datasetForFetch-shaped helper, mirroring the worker's caching logic so the
// call-once contract can be unit-tested without the Flight-laden FetchWorker.
class FetchSession {
 public:
  PJ::Expected<PJ::sdk::DataSourceHandle> datasetForFetch(
      const PJ::sdk::ToolboxHostView& host, const std::string& sequence_name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!handle_.has_value()) {
      auto ds = host.createDataSource(sequence_name);
      if (!ds) {
        return PJ::unexpected(std::move(ds).error());
      }
      handle_ = *ds;
    }
    return *handle_;
  }
  void reset() {
    std::lock_guard<std::mutex> lk(mu_);
    handle_ = std::nullopt;
  }

 private:
  std::optional<PJ::sdk::DataSourceHandle> handle_;
  std::mutex mu_;
};

}  // namespace

// datasetForFetch must create the source exactly once even when called
// concurrently — the parallel pullTopics callbacks race here.
TEST(FetchGrouping, DatasetForFetchCreatesOnceUnderConcurrency) {
  FakeHost fake;
  auto host = fake.view();
  FetchSession session;

  constexpr int kThreads = 8;
  std::vector<std::thread> workers;
  std::vector<PJ::sdk::DataSourceHandle> handles(kThreads);
  std::vector<bool> oks(kThreads, false);
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&, i] {
      auto ds = session.datasetForFetch(host, "seq");
      if (ds) {
        handles[static_cast<std::size_t>(i)] = *ds;
        oks[static_cast<std::size_t>(i)] = true;
      }
    });
  }
  for (auto& t : workers) {
    t.join();
  }

  // Exactly one data source created despite the concurrent callers.
  ASSERT_EQ(fake.data_sources.size(), 1u);
  const std::uint32_t source_id = fake.data_sources[0].id;
  for (int i = 0; i < kThreads; ++i) {
    EXPECT_TRUE(oks[static_cast<std::size_t>(i)]);
    EXPECT_EQ(handles[static_cast<std::size_t>(i)].id, source_id);
  }
}
