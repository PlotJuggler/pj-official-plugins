// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace pj::streaming {

/// Mutex-protected, self-coalescing handoff. Writers replace the pending value;
/// the reader atomically takes the latest value and empties the slot.
template <typename T>
class LatestValueSlot {
 public:
  void set(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = std::move(value);
  }

  [[nodiscard]] std::optional<T> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<T> result;
    result.swap(value_);
    return result;
  }

 private:
  std::mutex mutex_;
  std::optional<T> value_;
};

/// Convert ABI-style string views (members `data` and `size`) to the full
/// declarative topic set delivered by pj.topic_subscription.v1.
template <typename StringView, typename Size>
[[nodiscard]] std::set<std::string> stringSetFromViews(const StringView* values, Size count) {
  std::set<std::string> result;
  for (Size i = 0; i < count; ++i) {
    result.emplace(values[i].data, values[i].size);
  }
  return result;
}

}  // namespace pj::streaming
