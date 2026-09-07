// SPDX-License-Identifier: MIT

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/ingest_completion.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "arrow_ipc_message.hpp"
#include "fetch_worker.hpp"

namespace mosaico::testing {

class FetchWorkerTestAccess {
 public:
  /// Replaces the Flight pull; @p pull_topics is handed the worker's own
  /// per-batch and per-topic-done callbacks to drive the ingest path directly.
  static void setPullTopicsOverride(
      FetchWorker& worker, std::function<void(const FetchWorker::OnBatch&, const FetchWorker::OnDone&)> pull_topics) {
    worker.pull_topics_override_ = std::move(pull_topics);
  }

  static void setCancelActivePullsOverride(FetchWorker& worker, std::function<void()> cancel_active_pulls) {
    worker.cancel_active_pulls_override_ = std::move(cancel_active_pulls);
  }

  /// Seed the origin normally extracted by connectAsync, which unit tests
  /// cannot call without a live Flight endpoint.
  static void setServerOrigin(FetchWorker& worker, std::string origin) {
    worker.server_origin_ = std::move(origin);
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
    ingest_vtable_.request_stop = &FakeIngestHost::requestStop;
    ingest_vtable_.attach_source_record = &FakeIngestHost::attachSourceRecord;
    ingest_vtable_.complete_ingest = &FakeIngestHost::completeIngest;

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

  /// Shrink the ingest vtable to the pre-0.30 layout: attach_source_record is
  /// still visible but complete_ingest falls past struct_size.
  void emulateHostWithoutCompleteIngest() {
    ingest_vtable_.struct_size = offsetof(PJ_data_source_runtime_host_vtable_t, complete_ingest);
  }

  void failParserIngestCreation() {
    create_ingest_succeeds_.store(false);
  }

  void failProgressStart() {
    progress_start_succeeds_.store(false);
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
  std::vector<Binding> bindings;                                              // guarded by mu
  std::vector<Message> messages;                                              // guarded by mu
  std::vector<std::string> attached_records;                                  // guarded by mu
  std::vector<std::pair<PJ_data_source_state_t, std::string>> stop_requests;  // guarded by mu
  std::vector<PJ::sdk::IngestCompletionRecord> completions;                   // guarded by mu (validated copies)
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
    return static_cast<FakeIngestHost*>(ctx)->progress_start_succeeds_.load();
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

  static void requestStop(void* ctx, PJ_data_source_state_t terminal_state, PJ_string_view_t reason) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    std::lock_guard<std::mutex> lock(self->mu);
    self->stop_requests.emplace_back(terminal_state, str(reason));
  }

  static bool attachSourceRecord(void* ctx, PJ_string_view_t descriptor_json, PJ_error_t*) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    std::lock_guard<std::mutex> lock(self->mu);
    self->attached_records.push_back(str(descriptor_json));
    return true;
  }

  // Runs the SDK's own fail-closed validator, so a malformed completion fails
  // the test instead of being silently recorded.
  static bool completeIngest(void* ctx, const PJ_ingest_completion_t* completion, PJ_error_t* error) PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    auto record = PJ::sdk::copyIngestCompletion(completion);
    if (!record) {
      if (error != nullptr) {
        PJ::sdk::setErrorField(error->message, sizeof(error->message), record.error().c_str());
      }
      return false;
    }
    std::lock_guard<std::mutex> lock(self->mu);
    self->completions.push_back(std::move(*record));
    return true;
  }

  static bool createParserIngest(void* ctx, std::uint32_t, PJ_data_source_runtime_host_t* out_host, PJ_error_t* error)
      PJ_NOEXCEPT {
    auto* self = static_cast<FakeIngestHost*>(ctx);
    if (!self->create_ingest_succeeds_.load()) {
      PJ::sdk::setErrorField(error->message, sizeof(error->message), "no parser installed for arrow-ipc");
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
  std::atomic<bool> progress_start_succeeds_{true};
  std::atomic<bool> live_{false};
  std::atomic<bool> stop_requested_{false};
};

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> arrayOf(const std::vector<Value>& values) {
  Builder builder;
  for (const auto& value : values) {
    EXPECT_TRUE(builder.Append(value).ok());
  }
  std::shared_ptr<arrow::Array> array;
  EXPECT_TRUE(builder.Finish(&array).ok());
  return array;
}

// Minimal extension type: only its storage matters to the rewrite.
class StructExtensionType : public arrow::ExtensionType {
 public:
  explicit StructExtensionType(std::shared_ptr<arrow::DataType> storage) : arrow::ExtensionType(std::move(storage)) {}
  std::string extension_name() const override {
    return "mosaico.test.stamp";
  }
  bool ExtensionEquals(const arrow::ExtensionType& other) const override {
    return other.extension_name() == extension_name() && other.storage_type()->Equals(*storage_type());
  }
  std::shared_ptr<arrow::Array> MakeArray(std::shared_ptr<arrow::ArrayData> data) const override {
    return std::make_shared<arrow::ExtensionArray>(std::move(data));
  }
  arrow::Result<std::shared_ptr<arrow::DataType>> Deserialize(
      std::shared_ptr<arrow::DataType> storage_type, const std::string&) const override {
    return std::make_shared<StructExtensionType>(std::move(storage_type));
  }
  std::string Serialize() const override {
    return {};
  }
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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&](auto&, auto&) {
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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&](auto&, auto&) {
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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](auto&, auto&) {});

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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](auto&, auto&) {});

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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&host](auto&, auto&) {
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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_batch("gps/fix", scalarBatch(2000));
    on_done("gps/fix", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(host.bindings[0].topic, "gps/fix");
  EXPECT_EQ(host.bindings[0].encoding, "arrow-ipc");
  EXPECT_EQ(host.bindings[0].type_name, "");  // no ontology tag cached or in the schema
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
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  EXPECT_TRUE(host.messages.empty());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_FALSE(results[0].ok);
  // The host's own reason reaches the user, not a hardcoded placeholder.
  EXPECT_NE(results[0].error.find("no parser installed for arrow-ipc"), std::string::npos) << results[0].error;
  EXPECT_EQ(host.discardedParserIngests(), 1U);
}

// A refused progressStart costs the progress bar only: the ingest context is
// live, so the topic must still import rather than fail with a blank reason.
TEST(MosaicoTransport, FailedProgressStartStillImportsScalarTopics) {
  FakeIngestHost host;
  host.failProgressStart();
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });

  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_EQ(host.messages.size(), 1U);
  // No progressStart means no progressFinish: the lifecycle stays paired.
  EXPECT_EQ(host.progressFinishes(), 0U);
  EXPECT_EQ(host.releasedParserIngests(), 1U);
}

std::shared_ptr<arrow::RecordBatch> unstampedBatch(int rows = 3) {
  arrow::DoubleBuilder value_builder;
  for (int row = 0; row < rows; ++row) {
    EXPECT_TRUE(value_builder.Append(static_cast<double>(row)).ok());
  }
  std::shared_ptr<arrow::Array> value_array;
  EXPECT_TRUE(value_builder.Finish(&value_array).ok());
  return arrow::RecordBatch::Make(arrow::schema({arrow::field("value", arrow::float64())}), rows, {value_array});
}

// Drive one timestamp-less topic through the worker: one batch per entry in
// @p batch_rows, with the topic's [min,max] range cached as its TopicInfo.
void pullUnstamped(FakeIngestHost& host, std::int64_t min_ts_ns, std::int64_t max_ts_ns, std::vector<int> batch_rows) {
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::TopicInfo info;
  info.topic_name = "imu/raw";
  info.min_ts_ns = min_ts_ns;
  info.max_ts_ns = max_ts_ns;
  worker.setTopicInfoCache({{"imu/raw", info}});
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch_rows](const auto& on_batch, const auto& on_done) {
        for (int rows : batch_rows) {
          on_batch("imu/raw", unstampedBatch(rows));
        }
        on_done("imu/raw", mosaico::PullResult{});
      });
  worker.pullTopicsAsync("seq", {"imu/raw"}, 0, 1);
}

std::int64_t boundInterval(const FakeIngestHost& host) {
  return nlohmann::json::parse(host.bindings[0].config).at("synthetic_interval_ns").get<std::int64_t>();
}

// Rows without a timestamp column get a synthetic axis fitted to the topic's
// [min,max] range — which needs the total row count, so those batches are held
// back and pushed together once the topic completes. The parser is told the
// same interval so the axis continues within a batch.
TEST(MosaicoTransport, UnstampedTopicFitsItsSyntheticCadenceToTheTopicRange) {
  FakeIngestHost host;
  pullUnstamped(host, 1000, 6000, {3, 3});

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "__mosaico_timestamp");
  EXPECT_EQ(boundInterval(host), 0) << "6 rows over [1000, 6000]";
  ASSERT_EQ(host.messages.size(), 2U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 1000);
  EXPECT_EQ(host.messages[1].host_ts_ns, 4000);
}

// Nothing to fit: the ~30 fps default carries the axis, exactly as before the
// range was consulted.
TEST(MosaicoTransport, UnstampedTopicFallsBackToTheDefaultCadence) {
  FakeIngestHost zero_span;
  pullUnstamped(zero_span, 5000, 5000, {1, 1});
  ASSERT_EQ(zero_span.bindings.size(), 1U);
  EXPECT_EQ(boundInterval(zero_span), 0);
  ASSERT_EQ(zero_span.messages.size(), 2U);
  EXPECT_EQ(zero_span.messages[0].host_ts_ns, 5000);
  EXPECT_EQ(zero_span.messages[1].host_ts_ns, 5000 + mosaico::kSyntheticIntervalNs);

  // A single row spans nothing however wide the topic's range — dividing by
  // (rows - 1) would be a division by zero.
  FakeIngestHost one_row;
  pullUnstamped(one_row, 1000, 6000, {1});
  ASSERT_EQ(one_row.bindings.size(), 1U);
  EXPECT_EQ(boundInterval(one_row), 0);
  ASSERT_EQ(one_row.messages.size(), 1U);
  EXPECT_EQ(one_row.messages[0].host_ts_ns, 1000);
}

// A column named `time` whose type parser_arrow will not accept as an axis
// (utf8 here) is not this topic's timestamp. Naming it in the config makes the
// parser refuse the topic outright; left undetected, the topic imports on the
// fitted synthetic cadence and keeps the column as an ordinary curve.
TEST(MosaicoTransport, ImplausiblyTypedTimeColumnFallsBackToTheSyntheticAxis) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::TopicInfo info;
  info.topic_name = "log";
  info.min_ts_ns = 1000;
  info.max_ts_ns = 3000;
  worker.setTopicInfoCache({{"log", info}});

  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    auto schema = arrow::schema({arrow::field("time", arrow::utf8()), arrow::field("value", arrow::float64())});
    on_batch(
        "log", arrow::RecordBatch::Make(
                   schema, 3,
                   {arrayOf<arrow::StringBuilder, std::string>({"a", "b", "c"}),
                    arrayOf<arrow::DoubleBuilder, double>({0.0, 1.0, 2.0})}));
    on_done("log", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"log"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "__mosaico_timestamp");
  EXPECT_EQ(boundInterval(host), 0) << "3 rows over [1000, 3000]";
  ASSERT_EQ(host.messages.size(), 1U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 1000);
}

std::shared_ptr<arrow::RecordBatch> nestedStampBatch(std::int64_t first_stamp_us) {
  arrow::TimestampBuilder stamp_builder(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
  arrow::StringBuilder frame_builder;
  arrow::DoubleBuilder value_builder;
  for (int row = 0; row < 3; ++row) {
    EXPECT_TRUE(stamp_builder.Append(first_stamp_us + row).ok());
    EXPECT_TRUE(frame_builder.Append("map").ok());
    EXPECT_TRUE(value_builder.Append(static_cast<double>(row)).ok());
  }
  std::shared_ptr<arrow::Array> stamps;
  std::shared_ptr<arrow::Array> frames;
  std::shared_ptr<arrow::Array> values;
  EXPECT_TRUE(stamp_builder.Finish(&stamps).ok());
  EXPECT_TRUE(frame_builder.Finish(&frames).ok());
  EXPECT_TRUE(value_builder.Finish(&values).ok());
  auto header =
      *arrow::StructArray::Make(arrow::ArrayVector{frames, stamps}, std::vector<std::string>{"frame_id", "stamp"});
  auto schema = arrow::schema({arrow::field("value", arrow::float64()), arrow::field("header", header->type())});
  return arrow::RecordBatch::Make(schema, 3, {values, std::static_pointer_cast<arrow::Array>(header)});
}

// A ROS-shaped topic carries its stamp inside `header`, with no top-level
// timestamp column. Detection walks the flattened leaves, so the topic stays on
// the real time axis instead of silently falling back to a synthetic one.
TEST(MosaicoTransport, NestedStampTopicBindsTheFlattenedLeafPath) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("odom", nestedStampBatch(7000));
    on_done("odom", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "header/stamp");
  ASSERT_EQ(host.messages.size(), 1U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 7'000'000LL);
}

// The stamp only looks like a timestamp AFTER the IPC rewrite: detecting on the
// raw schema would call this topic unstamped and invent a synthetic axis while
// the parser happily used the real column.
TEST(MosaicoTransport, DictionaryEncodedStampIsDetectedAfterTheRewrite) {
  arrow::TimestampBuilder stamp_builder(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
  arrow::DoubleBuilder value_builder;
  for (int row = 0; row < 3; ++row) {
    EXPECT_TRUE(stamp_builder.Append(7000 + row).ok());
    EXPECT_TRUE(value_builder.Append(static_cast<double>(row)).ok());
  }
  std::shared_ptr<arrow::Array> stamps;
  std::shared_ptr<arrow::Array> values;
  ASSERT_TRUE(stamp_builder.Finish(&stamps).ok());
  ASSERT_TRUE(value_builder.Finish(&values).ok());
  auto dictionary_type = arrow::dictionary(arrow::int32(), stamps->type());
  auto encoded = arrow::DictionaryArray::FromArrays(
      dictionary_type, arrayOf<arrow::Int32Builder, std::int32_t>({0, 1, 2}), stamps);
  ASSERT_TRUE(encoded.ok()) << encoded.status().ToString();
  auto schema = arrow::schema({arrow::field("value", arrow::float64()), arrow::field("stamp", dictionary_type)});
  auto batch = arrow::RecordBatch::Make(schema, 3, {values, *encoded});

  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch](const auto& on_batch, const auto& on_done) {
        on_batch("gps/fix", batch);
        on_done("gps/fix", mosaico::PullResult{});
      });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "stamp");
  ASSERT_EQ(host.messages.size(), 1U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 7'000'000LL) << "stamped from the decoded value, not synthesized";
}

// An undecodable column costs its own curves, not the topic: the siblings still
// import and the loss is reported as a warning on the successful result.
TEST(MosaicoTransport, UnsupportedPlotColumnIsPreservedOnTheWire) {
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2, 3});
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, ids);
  auto stamps = arrayOf<arrow::Int64Builder, std::int64_t>({1000, 1001, 1002});
  // The dropped column sits BEFORE the timestamp on purpose: the axis lands at
  // index 1 raw and index 0 framed, so reading the stamp from the raw batch
  // would yield nullopt and silently synthesize instead.
  auto schema = arrow::schema(
      {arrow::field("counter", ree->type()), arrow::field("timestamp_ns", arrow::int64()),
       arrow::field("value", arrow::int64())});
  auto batch = arrow::RecordBatch::Make(schema, 3, {ree, stamps, ids});

  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch](const auto& on_batch, const auto& on_done) {
        on_batch("odom", batch);
        on_done("odom", mosaico::PullResult{});
      });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_TRUE(results[0].warning.empty());
  ASSERT_EQ(host.messages.size(), 1U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 1000) << "the surviving stamp still drives the axis";
}

// Losing the axis is the one drop that is fatal: every surviving curve would
// land on a synthetic base that disagrees with the rest of the sequence.
TEST(MosaicoTransport, UnplottableAxisRemainsInTheRecordedMessage) {
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2, 3});
  // A run-end encoded `timestamp_ns` cannot be framed, so the axis is gone.
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, ids);
  auto schema = arrow::schema({arrow::field("timestamp_ns", ree->type()), arrow::field("value", arrow::int64())});
  auto batch = arrow::RecordBatch::Make(schema, 3, {ree, ids});

  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch](const auto& on_batch, const auto& on_done) {
        on_batch("odom", batch);
        on_done("odom", mosaico::PullResult{});
      });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  EXPECT_EQ(host.messages.size(), 1U);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
}

// A struct must keep the children that CAN be framed — including the axis. The
// unframeable sibling costs only itself, so the topic still rides `hdr/stamp`
// rather than falling back to a synthetic base or dying outright.
TEST(MosaicoTransport, KeepsAllStructChildrenAndTheirAxis) {
  auto ids = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto run_ends = arrayOf<arrow::Int32Builder, std::int32_t>({1, 2, 3});
  auto ree = *arrow::RunEndEncodedArray::Make(3, run_ends, ids);
  arrow::TimestampBuilder stamp_builder(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
  for (int row = 0; row < 3; ++row) {
    ASSERT_TRUE(stamp_builder.Append(10 + row).ok());
  }
  std::shared_ptr<arrow::Array> stamps;
  ASSERT_TRUE(stamp_builder.Finish(&stamps).ok());
  auto values = arrayOf<arrow::DoubleBuilder, double>({0.5, 1.5, 2.5});
  auto hdr = *arrow::StructArray::Make(
      arrow::ArrayVector{stamps, values, ree}, std::vector<std::string>{"stamp", "value", "weird"});
  auto schema = arrow::schema({arrow::field("hdr", hdr->type()), arrow::field("x", arrow::float64())});
  auto batch = arrow::RecordBatch::Make(
      schema, 3, {std::static_pointer_cast<arrow::Array>(hdr), arrayOf<arrow::DoubleBuilder, double>({0, 1, 2})});

  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch](const auto& on_batch, const auto& on_done) {
        on_batch("odom", batch);
        on_done("odom", mosaico::PullResult{});
      });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_TRUE(results[0].warning.empty());
  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(nlohmann::json::parse(host.bindings[0].config).at("timestamp_column"), "hdr/stamp");
  ASSERT_EQ(host.messages.size(), 1U);
  EXPECT_EQ(host.messages[0].host_ts_ns, 10) << "read through the PROJECTED struct, not the raw one";

  // The siblings really are on the wire, under the projected shape.
  auto buffer = std::make_shared<arrow::Buffer>(
      host.messages[0].payload.data(), static_cast<std::int64_t>(host.messages[0].payload.size()));
  auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buffer));
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  const auto& framed = *(*reader)->schema();
  ASSERT_EQ(framed.num_fields(), 2);
  ASSERT_EQ(framed.field(0)->type()->num_fields(), 3);
  EXPECT_EQ(framed.field(0)->type()->field(0)->name(), "stamp");
  EXPECT_EQ(framed.field(0)->type()->field(1)->name(), "value");
  EXPECT_EQ(framed.field(1)->name(), "x");
}

// The raw schema had one leaf named `time`; the rewrite turned it into `time/sec`
// and `time/nsec`, so the axis is gone without a single column being dropped.
// Judging that only when something was dropped would miss it.
TEST(MosaicoTransport, ExtensionFieldsRemainInTheRecordedMessage) {
  auto secs = arrayOf<arrow::Int64Builder, std::int64_t>({1, 2, 3});
  auto nsecs = arrayOf<arrow::Int64Builder, std::int64_t>({4, 5, 6});
  auto storage = *arrow::StructArray::Make(arrow::ArrayVector{secs, nsecs}, std::vector<std::string>{"sec", "nsec"});
  std::shared_ptr<arrow::DataType> stamp_type = std::make_shared<StructExtensionType>(storage->type());
  auto schema = arrow::schema({arrow::field("time", stamp_type), arrow::field("value", arrow::float64())});
  auto batch = arrow::RecordBatch::Make(
      schema, 3,
      {arrow::ExtensionType::WrapArray(stamp_type, storage), arrayOf<arrow::DoubleBuilder, double>({0, 1, 2})});

  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&batch](const auto& on_batch, const auto& on_done) {
        on_batch("odom", batch);
        on_done("odom", mosaico::PullResult{});
      });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  EXPECT_EQ(host.messages.size(), 1U);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
}

// A synthetic axis anchored near INT64_MAX would silently wrap into the distant
// past; the topic fails naming the overflow instead.
TEST(MosaicoTransport, SyntheticTimestampOverflowFailsTheTopic) {
  FakeIngestHost host;
  std::vector<mosaico::PullResultEvent> results;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::TopicInfo info;
  info.topic_name = "imu/raw";
  info.min_ts_ns = std::numeric_limits<std::int64_t>::max() - 10;
  worker.setTopicInfoCache({{"imu/raw", info}});
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("imu/raw", unstampedBatch(2));
    on_batch("imu/raw", unstampedBatch(2));
    on_done("imu/raw", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"imu/raw"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_FALSE(results[0].ok);
  EXPECT_NE(results[0].error.find("overflow"), std::string::npos) << results[0].error;
}

// Canonical-object ontologies stay on the direct-write path: no parser binding,
// no arrow-ipc message.
TEST(MosaicoTransport, ObjectOntologyUsesOneRawMessagePerRow) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::TopicInfo info;
  info.topic_name = "odom";
  info.ontology_tag = "pose";
  worker.setTopicInfoCache({{"odom", info}});
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("odom", scalarBatch(1000));
    on_done("odom", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"odom"}, 0, 1);

  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(host.bindings[0].type_name, "pose");
  EXPECT_EQ(host.bindings[0].encoding, "arrow-ipc");
  ASSERT_EQ(host.messages.size(), 3U);
  for (std::size_t row = 0; row < host.messages.size(); ++row) {
    EXPECT_EQ(host.messages[row].host_ts_ns, 1000 + row);
    auto buffer = std::make_shared<arrow::Buffer>(host.messages[row].payload.data(), host.messages[row].payload.size());
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buffer));
    ASSERT_TRUE(reader.ok());
    auto batch = (*reader)->Next();
    ASSERT_TRUE(batch.ok());
    ASSERT_EQ((*batch)->num_rows(), 1);
    EXPECT_EQ((*batch)->num_columns(), 2);
  }
}

// ---------------------------------------------------------------------------
// Source capture (SDK 0.30): the request descriptor + the completion terminal.
// ---------------------------------------------------------------------------

// The descriptor is the byte-exact identity a re-download must reproduce:
// alphabetical keys (nlohmann's ordering), topics sorted and deduped, and an
// origin carrying host:port only — never a scheme, credentials, or path.
TEST(MosaicoSourceCapture, DescriptorIsAttachedOnceAndCanonical) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("b/two", scalarBatch(1000));
    on_done("b/two", mosaico::PullResult{});
    on_batch("a/one", scalarBatch(2000));
    on_done("a/one", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"b/two", "a/one", "b/two"}, 100, 200);

  ASSERT_EQ(host.attached_records.size(), 1U);
  EXPECT_EQ(
      host.attached_records[0],
      R"({"kind":"mosaico.pull","request":{"end_ns":"200","origin":"mosaico.example.com:32010",)"
      R"("sequence":"seq","start_ns":"100","topics":["a/one","b/two"]},"v":1})");
}

TEST(MosaicoSourceCapture, AllTopicsSucceedingReportsCompleted) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kCompleted);
  EXPECT_FALSE(host.completions[0].attestsEmptyTopics());
  EXPECT_EQ(host.completions[0].requested_topics, std::vector<std::string>{"gps/fix"});
}

TEST(MosaicoSourceCapture, CancelledDownloadReportsCancelled) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setCancelActivePullsOverride(worker, [] {});
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [&worker](const auto&, const auto&) {
    worker.requestCancel();  // Cancel arrives mid-transport; no topic completes.
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kCancelled);
  EXPECT_FALSE(host.completions[0].attestsEmptyTopics());
  // The stop reached the host, with the TERMINAL state (kStopped, not
  // kStopping): the cancelled verdict travels via the completion, not the state.
  ASSERT_EQ(host.stop_requests.size(), 1U);
  EXPECT_EQ(host.stop_requests[0].first, PJ_DATA_SOURCE_STATE_STOPPED);
  EXPECT_EQ(host.stop_requests[0].second, "download cancelled");
}

// Every non-authority URI component is credential-bearing or identity-noise;
// only a lowercased host:port may reach the descriptor.
TEST(MosaicoSourceCapture, OriginFromUriKeepsOnlyLowercasedHostPort) {
  EXPECT_EQ(mosaico::originFromUri("grpc+tls://example.com:6726?api_key=secret"), "example.com:6726");
  EXPECT_EQ(mosaico::originFromUri("grpc://user:p@ss@EXAMPLE.com:6726/path?k=v#frag"), "example.com:6726");
  EXPECT_EQ(mosaico::originFromUri("grpc+tls://Example.com:6726#fragment"), "example.com:6726");
  EXPECT_EQ(mosaico::originFromUri("example.com:6726"), "example.com:6726");
}

// Sequence and topic names are server-supplied bytes; a name that is not
// valid UTF-8 cannot be represented without renaming the request (a U+FFFD
// substitution would alias a different, legitimately-named request), so the
// batch gets NO source record and NO completion — imported eagerly, never
// cacheable. Must not throw either way.
TEST(MosaicoSourceCapture, InvalidUtf8NamesProduceNoRecordAndNoCompletion) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto&, const auto&) {});

  worker.pullTopicsAsync("seq\xFFname", {"bad\xFFtopic"}, 0, 1);

  EXPECT_TRUE(host.attached_records.empty());
  EXPECT_TRUE(host.completions.empty());
}

// One failed topic poisons the whole-request attestation, even though the
// sibling imported fine.
TEST(MosaicoSourceCapture, FailedTopicReportsFailedForTheWholeRequest) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
    on_done("imu/raw", arrow::Status::IOError("stream reset"));
  });

  worker.pullTopicsAsync("seq", {"gps/fix", "imu/raw"}, 0, 1);

  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kFailed);
  const std::vector<std::string> expected{"gps/fix", "imu/raw"};
  EXPECT_EQ(host.completions[0].requested_topics, expected) << "always the FULL requested set";
}

// A transport-successful fetch of an empty window is a success with a warning,
// and the completion attests the emptiness so the request stays cacheable.
TEST(MosaicoSourceCapture, EmptyTopicReportsCompletedWithEmptyAttestation) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto&, const auto& on_done) {
    on_done("gps/fix", mosaico::PullResult{});  // no batches: the window was empty
  });

  bool discard_signalled = false;
  worker.datasetDiscarded = [&discard_signalled] { discard_signalled = true; };

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_NE(results[0].warning.find("no data"), std::string::npos) << results[0].warning;
  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kCompleted);
  EXPECT_TRUE(host.completions[0].attestsEmptyTopics());
  // Nothing was imported, so the provisional dataset rolls back: the user gets
  // no stray empty dataset even though the attestation stays cacheable — and
  // the rollback is SIGNALLED so a provider terminal never claims the handle.
  EXPECT_EQ(host.discardedParserIngests(), 1U);
  EXPECT_TRUE(discard_signalled);
}

// One topic with rows, one genuinely empty: the dataset is kept (real data
// landed) and the completion still attests the empty sibling.
TEST(MosaicoSourceCapture, MixedEmptyAndDataTopicsKeepDatasetAndAttest) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
    on_done("imu/raw", mosaico::PullResult{});  // empty window
  });

  worker.pullTopicsAsync("seq", {"gps/fix", "imu/raw"}, 0, 1);

  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kCompleted);
  EXPECT_TRUE(host.completions[0].attestsEmptyTopics());
  EXPECT_EQ(host.discardedParserIngests(), 0U);
}

// A host stop that lands after the poller's last check must still veto the
// COMPLETED attestation: the terminal reads the LIVE host stop flag.
TEST(MosaicoSourceCapture, HostStopAfterLastPollReportsCancelled) {
  FakeIngestHost host;
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  mosaico::testing::FetchWorkerTestAccess::setCancelActivePullsOverride(worker, [] {});
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(
      worker, [&host](const auto& on_batch, const auto& on_done) {
        on_batch("gps/fix", scalarBatch(1000));
        on_done("gps/fix", mosaico::PullResult{});
        host.requestStopFromHost();  // too late for the 50 ms poller
      });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(host.completions.size(), 1U);
  EXPECT_EQ(host.completions[0].outcome, PJ::sdk::IngestOutcome::kCancelled);
}

// A pre-0.30 host still receives the descriptor but has no complete_ingest
// slot: the download must succeed exactly as before, just uncached.
TEST(MosaicoSourceCapture, HostWithoutCompleteIngestIsTolerated) {
  FakeIngestHost host;
  host.emulateHostWithoutCompleteIngest();
  mosaico::FetchWorker worker;
  worker.setHostProvider([&host] { return host.writeView(); });
  worker.setRuntimeHostProvider([&host] { return host.runtimeView(); });
  mosaico::testing::FetchWorkerTestAccess::setServerOrigin(worker, "mosaico.example.com:32010");
  std::vector<mosaico::PullResultEvent> results;
  worker.pullFinished = [&results](mosaico::PullResultEvent result) { results.push_back(std::move(result)); };
  mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(worker, [](const auto& on_batch, const auto& on_done) {
    on_batch("gps/fix", scalarBatch(1000));
    on_done("gps/fix", mosaico::PullResult{});
  });

  worker.pullTopicsAsync("seq", {"gps/fix"}, 0, 1);

  ASSERT_EQ(results.size(), 1U);
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_EQ(host.attached_records.size(), 1U);
  EXPECT_TRUE(host.completions.empty());
}

}  // namespace

TEST(MosaicoTransport, FitsSyntheticCadenceAcrossASpanLargerThanInt64) {
  FakeIngestHost host;
  const auto first = std::numeric_limits<std::int64_t>::min();
  pullUnstamped(host, first, 0, {1, 1, 1, 1});
  ASSERT_EQ(host.bindings.size(), 1U);
  EXPECT_EQ(boundInterval(host), 0);
  ASSERT_EQ(host.messages.size(), 4U);
  EXPECT_EQ(host.messages.front().host_ts_ns, first);
  EXPECT_EQ(host.messages.back().host_ts_ns, -2);  // Integral cadence rounds down.
}
