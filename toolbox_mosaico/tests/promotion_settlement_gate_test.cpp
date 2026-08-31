// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "promotion_settlement_gate.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

class SettlementOwner {
 public:
  explicit SettlementOwner(int* failures)
      : gate_(std::make_shared<mosaico::PromotionSettlementGate>([this, failures](std::string detail) {
          ++*failures;
          last_detail_ = std::move(detail);
        })) {}

  ~SettlementOwner() {
    gate_->invalidateAndWait();
  }

  std::shared_ptr<mosaico::PromotionSettlementGate> gate() const {
    return gate_;
  }

  const std::string& lastDetail() const {
    return last_detail_;
  }

 private:
  std::shared_ptr<mosaico::PromotionSettlementGate> gate_;
  std::string last_detail_;
};

TEST(PromotionSettlementGate, DelayedSettlementAfterOwnerDestructionIsIgnored) {
  int failures = 0;
  std::shared_ptr<mosaico::PromotionSettlementGate> retained_by_host;
  {
    SettlementOwner owner(&failures);
    retained_by_host = owner.gate();
    retained_by_host->settle(/*promoted=*/false, "before destruction");
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(owner.lastDetail(), "before destruction");
  }

  retained_by_host->settle(/*promoted=*/false, "delayed result");
  EXPECT_EQ(failures, 1);
}

TEST(PromotionSettlementGate, SuccessfulPromotionDoesNotReportFailure) {
  int failures = 0;
  SettlementOwner owner(&failures);
  owner.gate()->settle(/*promoted=*/true, "ignored");
  EXPECT_EQ(failures, 0);
}

TEST(PromotionSettlementGate, InvalidationWaitsForInFlightHandler) {
  std::mutex state_mu;
  std::condition_variable state_cv;
  bool handler_started = false;
  bool release_handler = false;
  bool invalidation_started = false;
  bool invalidation_returned = false;

  auto gate = std::make_shared<mosaico::PromotionSettlementGate>([&](std::string) {
    std::unique_lock<std::mutex> lock(state_mu);
    handler_started = true;
    state_cv.notify_all();
    state_cv.wait(lock, [&] { return release_handler; });
  });
  std::thread settlement([gate] { gate->settle(/*promoted=*/false, "in flight"); });

  {
    std::unique_lock<std::mutex> lock(state_mu);
    state_cv.wait(lock, [&] { return handler_started; });
  }
  std::thread invalidation([&] {
    {
      std::lock_guard<std::mutex> lock(state_mu);
      invalidation_started = true;
      state_cv.notify_all();
    }
    gate->invalidateAndWait();
    {
      std::lock_guard<std::mutex> lock(state_mu);
      invalidation_returned = true;
      state_cv.notify_all();
    }
  });

  {
    std::unique_lock<std::mutex> lock(state_mu);
    state_cv.wait(lock, [&] { return invalidation_started; });
    EXPECT_FALSE(state_cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return invalidation_returned; }));
    release_handler = true;
  }
  state_cv.notify_all();
  settlement.join();
  invalidation.join();
  EXPECT_TRUE(invalidation_returned);
}

}  // namespace
