// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace mosaico {

/// Lifetime barrier for a promotion result closure retained by the host.
/// invalidateAndWait() prevents new handler calls and waits for an in-flight
/// call to finish, so the handler may safely capture its owning dialog.
class PromotionSettlementGate {
 public:
  using FailureHandler = std::function<void(std::string)>;

  explicit PromotionSettlementGate(FailureHandler handler) : handler_(std::move(handler)) {}

  void settle(bool promoted, std::string detail) {
    if (promoted) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (handler_) {
      handler_(std::move(detail));
    }
  }

  void invalidateAndWait() {
    std::lock_guard<std::mutex> lock(mu_);
    handler_ = {};
  }

 private:
  std::mutex mu_;
  FailureHandler handler_;
};

}  // namespace mosaico
