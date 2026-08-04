#include "stoppable_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr auto kLongWait = 30s;  // never expected to elapse in a passing test
constexpr auto kShortWait = 20ms;

// The destructor must behave like std::jthread's: request stop, then join. A
// body that parks on waitForStop() has to come back and finish, or the
// destructor hangs.
TEST(StoppableThread, DestructorRequestsStopThenJoins) {
  std::atomic<bool> body_finished{false};
  {
    mosaico::StoppableThread poller([&](mosaico::StoppableThread& self) {
      while (!self.stopRequested()) {
        self.waitForStop(kLongWait);
      }
      body_finished.store(true);
    });
  }  // destructor: stop + join
  EXPECT_TRUE(body_finished.load());
}

// The reason the poller used the stop_token-aware wait_for overload: shutdown
// must not have to sit out the full poll interval. With a 30s interval, a
// destructor that only joined would take 30s.
TEST(StoppableThread, WaitForStopWakesImmediatelyOnStop) {
  std::atomic<bool> waiting{false};
  const auto start = std::chrono::steady_clock::now();
  {
    mosaico::StoppableThread poller([&](mosaico::StoppableThread& self) {
      waiting.store(true);
      self.waitForStop(kLongWait);
    });
    while (!waiting.load()) {
      std::this_thread::sleep_for(1ms);
    }
  }  // destructor wakes the wait rather than letting kLongWait elapse
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

// Returns true when stop was requested, false on timeout -- so a caller can
// distinguish "time to exit" from "poll again".
TEST(StoppableThread, WaitForStopReportsTimeoutVersusStop) {
  std::atomic<bool> timed_out_result{true};
  std::atomic<bool> saw_timeout{false};
  {
    mosaico::StoppableThread poller([&](mosaico::StoppableThread& self) {
      timed_out_result.store(self.waitForStop(kShortWait));  // expected: false (timeout)
      saw_timeout.store(true);
      while (!self.stopRequested()) {
        self.waitForStop(kLongWait);
      }
    });
    while (!saw_timeout.load()) {
      std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(timed_out_result.load());
  }
}

// A body that returns on its own must not make the destructor hang or abort.
TEST(StoppableThread, BodyMayReturnBeforeStopIsRequested) {
  std::atomic<int> runs{0};
  {
    mosaico::StoppableThread poller([&](mosaico::StoppableThread&) { runs.fetch_add(1); });
  }
  EXPECT_EQ(runs.load(), 1);
}

// requestStop() is idempotent and observable, so the poller loop can test it
// without racing the destructor.
TEST(StoppableThread, StopRequestedReflectsRequestStop) {
  std::atomic<bool> observed_before{true};
  mosaico::StoppableThread poller([&](mosaico::StoppableThread& self) {
    observed_before.store(self.stopRequested());
    while (!self.stopRequested()) {
      self.waitForStop(kLongWait);
    }
  });
  EXPECT_FALSE(poller.stopRequested());
  poller.requestStop();
  poller.requestStop();  // idempotent
  EXPECT_TRUE(poller.stopRequested());
}

}  // namespace
