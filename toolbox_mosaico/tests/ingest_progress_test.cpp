// SPDX-License-Identifier: MIT

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fetch_worker.hpp"

namespace mosaico::testing {

class FetchWorkerTestAccess {
 public:
  static void setPullTopicsOverride(FetchWorker& worker, std::function<void()> pull_topics) {
    worker.pull_topics_override_ = std::move(pull_topics);
  }

  static void setCancelActivePullsOverride(FetchWorker& worker, std::function<void()> cancel_active_pulls) {
    worker.cancel_active_pulls_override_ = std::move(cancel_active_pulls);
  }

  static void feedBatch(
      FetchWorker& worker, const std::string& topic, const std::shared_ptr<arrow::RecordBatch>& batch) {
    worker.on_batch_for_test_(topic, batch);
  }

  static void finishTopic(FetchWorker& worker, const std::string& topic) {
    worker.on_done_for_test_(topic, PullResult{});
  }
};

}  // namespace mosaico::testing

namespace {

using namespace std::chrono_literals;

#if defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
using FakeRuntimeVtable = PJ_toolbox_runtime_host_vtable_t;
#else
struct FakeRuntimeVtable {
  PJ_toolbox_runtime_host_vtable_t base;
  bool (*discard_parser_ingest)(void* ctx, std::uint32_t data_source_id, PJ_error_t* out_error) PJ_NOEXCEPT;
};
#endif

class FakeIngestHost {
 public:
  FakeIngestHost() {
    write_vtable_.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    write_vtable_.struct_size = sizeof(PJ_toolbox_host_vtable_t);
    write_vtable_.create_data_source = &FakeIngestHost::createDataSource;

    ingest_vtable_.protocol_version = PJ_DATA_SOURCE_PROTOCOL_VERSION;
    ingest_vtable_.struct_size = sizeof(PJ_data_source_runtime_host_vtable_t);
    ingest_vtable_.progress_start = &FakeIngestHost::progressStart;
    ingest_vtable_.progress_update = &FakeIngestHost::progressUpdate;
    ingest_vtable_.progress_finish = &FakeIngestHost::progressFinish;
    ingest_vtable_.is_stop_requested = &FakeIngestHost::isStopRequested;
    ingest_vtable_.ensure_parser_binding = &FakeIngestHost::ensureParserBinding;
    ingest_vtable_.push_message = &FakeIngestHost::pushMessage;

    auto& runtime_base = runtimeBase();
    runtime_base.protocol_version = PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION;
    runtime_base.struct_size = sizeof(FakeRuntimeVtable);
    runtime_base.create_parser_ingest = &FakeIngestHost::createParserIngest;
    runtime_base.release_parser_ingest = &FakeIngestHost::releaseParserIngest;
    runtime_vtable_.discard_parser_ingest = &FakeIngestHost::discardParserIngest;
  }

  [[nodiscard]] PJ::sdk::ToolboxHostView writeView() {
    return PJ::sdk::ToolboxHostView(PJ_toolbox_host_t{.ctx = this, .vtable = &write_vtable_});
  }

  [[nodiscard]] PJ::ToolboxRuntimeHostView runtimeView() {
    return PJ::ToolboxRuntimeHostView(PJ_toolbox_runtime_host_t{.ctx = this, .vtable = &runtimeBase()});
  }

  void requestStopFromHost() {
    stop_requested_.store(true);
  }

  [[nodiscard]] std::size_t createdParserIngests() const {
    return created_.load();
  }

  [[nodiscard]] std::size_t createdDataSources() const {
    return data_sources_created_.load();
  }

  [[nodiscard]] std::size_t liveDataSources() const {
    return live_data_sources_.load();
  }

  [[nodiscard]] std::size_t discardedParserIngests() const {
    return discarded_.load();
  }

  void emulateRuntimeWithoutDiscard() {
#if defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
    runtime_vtable_.struct_size = offsetof(PJ_toolbox_runtime_host_vtable_t, discard_parser_ingest);
#else
    runtimeBase().struct_size = sizeof(PJ_toolbox_runtime_host_vtable_t);
#endif
  }

  void failParserIngestCreation() {
    create_ingest_succeeds_.store(false);
  }

  [[nodiscard]] std::size_t releasedParserIngests() const {
    return released_.load();
  }

  [[nodiscard]] std::size_t progressFinishes() const {
    return progress_finishes_.load();
  }

  [[nodiscard]] bool hasLiveParserIngest() const {
    return live_.load();
  }

  struct Binding {
    std::string topic;
    std::string encoding;
    std::string type_name;
    std::vector<std::uint8_t> schema;
    std::string config;
  };
  struct Message {
    std::uint32_t binding = 0;
    std::int64_t host_ts_ns = 0;
    std::vector<std::uint8_t> payload;
  };
  std::vector<Binding> bindings;  // guarded by mu
  std::vector<Message> messages;  // guarded by mu
  std::mutex mu;

 private:
  PJ_toolbox_runtime_host_vtable_t& runtimeBase() {
#if defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
    return runtime_vtable_;
#else
    return runtime_vtable_.base;
#endif
  }

  static bool createDataSource(void* ctx, PJ_string_view_t, PJ_data_source_handle_t* out, PJ_error_t*) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    ++self->data_sources_created_;
    ++self->live_data_sources_;
    *out = PJ_data_source_handle_t{1};
    return true;
  }

  static bool progressStart(void* ctx, PJ_string_view_t, std::uint64_t, bool, PJ_error_t*) PJ_NOEXCEPT {
    static_cast<void>(ctx);
    return true;
  }

  static bool progressUpdate(void* ctx, std::uint64_t) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    return !self->stop_requested_.load();
  }

  static void progressFinish(void* ctx) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    ++self->progress_finishes_;
  }

  static bool isStopRequested(void* ctx) PJ_NOEXCEPT {
    return static_cast<FakeIngestHost*>(ctx)->stop_requested_.load();
  }

  static std::string str(PJ_string_view_t view) {
    return std::string(view.data, view.size);
  }

  static bool ensureParserBinding(
      void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out_handle,
      PJ_error_t*) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    std::lock_guard<std::mutex> lock(self->mu);
    self->bindings.push_back(
        Binding{
            str(request->topic_name), str(request->parser_encoding), str(request->type_name),
            std::vector<std::uint8_t>(request->schema.data, request->schema.data + request->schema.size),
            str(request->parser_config_json)});
    out_handle->id = static_cast<std::uint32_t>(self->bindings.size());
    return true;
  }

  static bool pushMessage(
      void* ctx, PJ_parser_binding_handle_t handle, std::int64_t host_timestamp_ns, PJ_message_data_fetcher_t fetch,
      PJ_error_t* error) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    PJ_payload_t payload{};
    const bool fetched = fetch.fetchMessageData(fetch.ctx, &payload, error);
    if (fetched) {
      std::lock_guard<std::mutex> lock(self->mu);
      self->messages.push_back(
          Message{handle.id, host_timestamp_ns, std::vector<std::uint8_t>(payload.data, payload.data + payload.size)});
      if (payload.anchor.release != nullptr) {
        payload.anchor.release(payload.anchor.ctx);
      }
    }
    if (fetch.release != nullptr) {
      fetch.release(fetch.ctx);
    }
    return fetched;
  }

  static bool createParserIngest(void* ctx, std::uint32_t, PJ_data_source_runtime_host_t* out_host, PJ_error_t*)
      PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    if (!self->create_ingest_succeeds_.load()) {
      return false;
    }
    ++self->created_;
    self->live_.store(true);
    *out_host = PJ_data_source_runtime_host_t{.ctx = self, .vtable = &self->ingest_vtable_};
    return true;
  }

  static bool releaseParserIngest(void* ctx, std::uint32_t, PJ_error_t*) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    ++self->released_;
    self->live_.store(false);
    return true;
  }

  static bool discardParserIngest(void* ctx, std::uint32_t data_source_id, PJ_error_t* error) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    (void)releaseParserIngest(ctx, data_source_id, error);
    ++self->discarded_;
    --self->live_data_sources_;
    return true;
  }

  PJ_toolbox_host_vtable_t write_vtable_{};
  PJ_data_source_runtime_host_vtable_t ingest_vtable_{};
  FakeRuntimeVtable runtime_vtable_{};
  std::atomic<std::size_t> data_sources_created_{0};
  std::atomic<std::size_t> live_data_sources_{0};
  std::atomic<std::size_t> created_{0};
  std::atomic<std::size_t> released_{0};
  std::atomic<std::size_t> discarded_{0};
  std::atomic<std::size_t> progress_finishes_{0};
  std::atomic<bool> create_ingest_succeeds_{true};
  std::atomic<bool> live_{false};
  std::atomic<bool> stop_requested_{false};
};

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// The host gates fold decisions on a live ingest context, so the context must
// precede transport dispatch and every possible topic completion.
TEST(MosaicoIngestProgress, ContextExistsBeforeFirstTopicCompletes) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });

  std::atomic<std::size_t> completed_topics{0};
  worker.pullFinished = [&completed_topics](mosaico::PullResultEvent) { ++completed_topics; };

  std::mutex pull_mu;
  std::condition_variable pull_cv;
  bool pull_entered = false;
  bool release_pull = false;
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&] {
    std::unique_lock<std::mutex> lock(pull_mu);
    pull_entered = true;
    pull_cv.notify_all();
    pull_cv.wait(lock, [&release_pull] { return release_pull; });
  });

  std::thread fetch_thread([&worker] { worker.pullTopicsAsync("seq", {"/a", "/b"}, 0, 1); });

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(pull_mu);
    entered = pull_cv.wait_for(lock, 2s, [&pull_entered] { return pull_entered; });
  }
  EXPECT_TRUE(entered);
  if (entered) {
    EXPECT_EQ(host.createdDataSources(), 1U);
    EXPECT_EQ(host.createdParserIngests(), 1U);
    EXPECT_TRUE(host.hasLiveParserIngest());
    EXPECT_EQ(completed_topics.load(), 0U);
  }

  {
    std::lock_guard<std::mutex> lock(pull_mu);
    release_pull = true;
  }
  pull_cv.notify_all();
  fetch_thread.join();

  EXPECT_EQ(host.releasedParserIngests(), 1U);
  EXPECT_EQ(host.progressFinishes(), 1U);
  EXPECT_FALSE(host.hasLiveParserIngest());
}

// Stop must actively interrupt the transport. Merely observing the atomic flag
// cannot release a real Flight reader already blocked inside Next().
TEST(MosaicoIngestProgress, StopInterruptsBlockedTransportRead) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });

  std::atomic<std::size_t> completed_topics{0};
  std::atomic<std::size_t> host_stop_callbacks{0};
  std::atomic<bool> active_cancel_called{false};
  worker.pullFinished = [&completed_topics](mosaico::PullResultEvent) { ++completed_topics; };
  worker.hostStopRequested = [&host_stop_callbacks] { ++host_stop_callbacks; };

  std::mutex pull_mu;
  std::condition_variable pull_cv;
  bool pull_entered = false;
  bool transport_released = false;
  mosaico::testing::FetchWorkerTestAccess::setCancelActivePullsOverride(worker, [&] {
    {
      std::lock_guard<std::mutex> lock(pull_mu);
      active_cancel_called.store(true);
      transport_released = true;
    }
    pull_cv.notify_all();
  });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&] {
    std::unique_lock<std::mutex> lock(pull_mu);
    pull_entered = true;
    pull_cv.notify_all();
    pull_cv.wait(lock, [&transport_released] { return transport_released; });
  });

  std::thread fetch_thread([&worker] { worker.pullTopicsAsync("seq", {"/a"}, 0, 1); });

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(pull_mu);
    entered = pull_cv.wait_for(lock, 2s, [&pull_entered] { return pull_entered; });
  }
  EXPECT_TRUE(entered);
  EXPECT_EQ(host.createdParserIngests(), 1U);
  EXPECT_TRUE(host.hasLiveParserIngest());
  EXPECT_EQ(completed_topics.load(), 0U);

  host.requestStopFromHost();
  const bool cancelled = waitUntil([&worker] { return worker.isCancelled(); }, 1s);
  EXPECT_TRUE(cancelled);
  fetch_thread.join();

  EXPECT_TRUE(active_cancel_called.load());
  EXPECT_EQ(host_stop_callbacks.load(), 1U);
  EXPECT_EQ(host.releasedParserIngests(), 1U);
  EXPECT_EQ(host.progressFinishes(), 1U);
  EXPECT_FALSE(host.hasLiveParserIngest());
}

TEST(MosaicoIngestProgress, ZeroSuccessBatchDiscardsProvisionalDataset) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [] {});

  worker.pullTopicsAsync("seq", {"/failed"}, 0, 1);

  EXPECT_EQ(host.createdDataSources(), 1U);
  EXPECT_EQ(host.discardedParserIngests(), 1U);
  EXPECT_EQ(host.liveDataSources(), 0U);
  EXPECT_FALSE(host.hasLiveParserIngest());
}

TEST(MosaicoIngestProgress, ZeroSuccessBatchDiscardsWhenProgressContextCreationFails) {
  FakeIngestHost host;
  host.failParserIngestCreation();
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [] {});

  worker.pullTopicsAsync("seq", {"/failed"}, 0, 1);

  EXPECT_EQ(host.createdDataSources(), 1U);
  EXPECT_EQ(host.createdParserIngests(), 0U);
  EXPECT_EQ(host.discardedParserIngests(), 1U);
  EXPECT_EQ(host.liveDataSources(), 0U);
}

TEST(MosaicoIngestProgress, OlderRuntimeDefersDatasetUntilFirstSuccess) {
  FakeIngestHost host;
  host.emulateRuntimeWithoutDiscard();
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&host] {
    EXPECT_EQ(host.createdDataSources(), 0U);
    EXPECT_EQ(host.createdParserIngests(), 0U);
  });

  worker.pullTopicsAsync("seq", {"/failed"}, 0, 1);

  EXPECT_EQ(host.createdDataSources(), 0U);
  EXPECT_EQ(host.liveDataSources(), 0U);
}

std::shared_ptr<arrow::RecordBatch> scalarBatch(std::int64_t first_ts) {
  arrow::Int64Builder ts_builder;
  arrow::DoubleBuilder value_builder;
  for (int row = 0; row < 3; ++row) {
    EXPECT_TRUE(ts_builder.Append(first_ts + row).ok());
    EXPECT_TRUE(value_builder.Append(static_cast<double>(row)).ok());
  }
  std::shared_ptr<arrow::Array> ts_array;
  std::shared_ptr<arrow::Array> value_array;
  EXPECT_TRUE(ts_builder.Finish(&ts_array).ok());
  EXPECT_TRUE(value_builder.Finish(&value_array).ok());
  auto schema = arrow::schema({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  return arrow::RecordBatch::Make(schema, 3, {ts_array, value_array});
}

// A scalar topic binds parser_arrow once and hands every Flight batch to the
// host as one complete arrow-ipc stream, stamped with its first row's time.
TEST(MosaicoTransport, ScalarTopicBindsOnceAndPushesOneIpcStreamPerBatch) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });

  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&] {
    mosaico::testing::FetchWorkerTestAccess::feedBatch(worker, "gps/fix", scalarBatch(1000));
    mosaico::testing::FetchWorkerTestAccess::feedBatch(worker, "gps/fix", scalarBatch(2000));
    mosaico::testing::FetchWorkerTestAccess::finishTopic(worker, "gps/fix");
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(host.bindings[0].topic, "gps/fix");
  EXPECT_EQ(host.bindings[0].encoding, "arrow-ipc");
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "timestamp_ns");
  EXPECT_FALSE(host.bindings[0].schema.empty());

  ASSERT_EQ(host.messages.size(), 2U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 1000);
  EXPECT_EQ(host.messages[1].host_ts_ns, 2000);
  for (const auto& message : host.messages) {
    EXPECT_EQ(message.binding, 1U);
    auto buffer =
        std::make_shared<arrow::Buffer>(message.payload.data(), static_cast<std::int64_t>(message.payload.size()));
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buffer));
    ASSERT_TRUE(reader.ok()) << reader.status().ToString();
    auto batch = (*reader)->Next();
    ASSERT_TRUE(batch.ok());
    ASSERT_NE(*batch, nullptr);
    EXPECT_EQ((*batch)->num_rows(), 3);
    EXPECT_EQ(*(*reader)->Next(), nullptr);
  }

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  // Data reached the host: the provisional dataset is kept, not discarded.
  EXPECT_EQ(host.discardedParserIngests(), 0U);
  EXPECT_EQ(host.releasedParserIngests(), 1U);
}

// Without a parser-ingest context (older host / parser_arrow absent) a scalar
// topic fails with a message instead of silently writing nothing.
TEST(MosaicoTransport, ScalarTopicFailsWhenTheHostOffersNoParserIngest) {
  FakeIngestHost host;
  host.failParserIngestCreation();
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });

  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&] {
    mosaico::testing::FetchWorkerTestAccess::feedBatch(worker, "gps/fix", scalarBatch(1000));
    mosaico::testing::FetchWorkerTestAccess::finishTopic(worker, "gps/fix");
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  EXPECT_TRUE(host.messages.empty());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_FALSE(results[0].ok);
  EXPECT_NE(results[0].error.find("parser ingest"), std::string::npos) << results[0].error;
  EXPECT_EQ(host.discardedParserIngests(), 1U);
}

}  // namespace
