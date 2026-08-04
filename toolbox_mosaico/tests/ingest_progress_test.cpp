// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

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

}  // namespace
