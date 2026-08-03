#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace mosaico {

/// A portable stand-in for the `std::jthread` + `std::stop_token` pair.
///
/// Apple's libc++ does not provide either (verified on Xcode 15.4 and 16.4), so
/// code using them compiles on Linux and Windows and fails on macOS. This offers
/// the two properties the codebase actually relies on:
///
///   1. the destructor requests stop and then joins, so the thread is bounded by
///      the enclosing scope on every exit path -- normal, early return, or an
///      exception unwinding through it;
///   2. `waitForStop()` returns as soon as stop is requested rather than sitting
///      out the remaining timeout, which is what the `std::condition_variable_any`
///      stop_token-aware `wait_for` overload provided.
///
/// The body receives the owning object, so it can poll `stopRequested()` and park
/// in `waitForStop()` without needing its own synchronisation.
class StoppableThread {
 public:
  template <typename Body>
  explicit StoppableThread(Body body) : thread_([this, body = std::move(body)]() mutable { body(*this); }) {}

  StoppableThread(const StoppableThread&) = delete;
  StoppableThread& operator=(const StoppableThread&) = delete;
  StoppableThread(StoppableThread&&) = delete;
  StoppableThread& operator=(StoppableThread&&) = delete;

  ~StoppableThread() {
    requestStop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// Idempotent. Wakes any thread parked in waitForStop().
  void requestStop() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] bool stopRequested() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
  }

  /// Blocks for at most `timeout`, returning early the moment stop is requested.
  /// Returns true if stop was requested, false if `timeout` elapsed first.
  template <typename Rep, typename Period>
  bool waitForStop(const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return stop_; });
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  // Declared last so the synchronisation members are fully constructed before
  // the thread that uses them starts.
  std::thread thread_;
};

}  // namespace mosaico
