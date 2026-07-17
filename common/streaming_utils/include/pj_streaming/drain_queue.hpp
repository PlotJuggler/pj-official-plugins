// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <mutex>
#include <queue>
#include <utility>

namespace pj::streaming {

/// Mutex-protected producer/consumer FIFO optimized for poll-loop consumers:
/// drain() swaps the complete pending batch out while holding the lock briefly.
template <typename T>
class DrainQueue {
 public:
  void push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(value));
  }

  [[nodiscard]] std::queue<T> drain() {
    std::queue<T> result;
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(result, queue_);
    return result;
  }

  void clear() {
    (void)drain();
  }

 private:
  std::mutex mutex_;
  std::queue<T> queue_;
};

}  // namespace pj::streaming
