// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "tool_context.hpp"
#include "tool_registry.hpp"

namespace assistant_agent {

// Marshals tool execution from any worker/MCP thread onto the GUI (host-callback)
// thread, where the host service views are legal to call. A caller submits a
// tool call and blocks until the dialog's onTick() drains + executes it, or a
// timeout elapses — a stalled/paused onTick must never hang the worker forever.
//
// Slots are shared_ptr so a timed-out caller can walk away while a later drain()
// still safely finds (and skips) the abandoned slot instead of touching freed
// memory.
class GuiExecutor {
 public:
  explicit GuiExecutor(std::chrono::milliseconds timeout = std::chrono::seconds(60)) : timeout_(timeout) {}

  // Worker/MCP thread: enqueue a tool call and block for its result.
  [[nodiscard]] ToolResult call(std::string name, nlohmann::json args) {
    auto slot = std::make_shared<Slot>();
    slot->name = std::move(name);
    slot->args = std::move(args);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (shutting_down_) {
        return ToolResult::failure("assistant is shutting down");
      }
      queue_.push_back(slot);
    }
    std::unique_lock<std::mutex> lk(slot->mu);
    if (!slot->cv.wait_for(lk, timeout_, [&] { return slot->done; })) {
      slot->abandoned = true;  // a later drain() must not fulfil a promise no one awaits
      return ToolResult::failure("tool '" + slot->name + "' timed out (the panel may be unresponsive)");
    }
    return std::move(slot->result);
  }

  // GUI thread (onTick): true when no call is pending — lets the tick skip
  // building a ToolContext (three provider calls + an allocation) in the
  // common idle case.
  [[nodiscard]] bool empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.empty();
  }

  // GUI thread (onTick): execute every pending call against `ctx`. Returns how
  // many ran, so the dialog can decide whether to re-render.
  std::size_t drain(const ToolRegistry& registry, ToolContext& ctx) {
    std::deque<std::shared_ptr<Slot>> batch;
    {
      std::lock_guard<std::mutex> lk(mu_);
      batch.swap(queue_);
    }
    for (auto& slot : batch) {
      ToolResult r = registry.execute(slot->name, slot->args, ctx);
      std::lock_guard<std::mutex> lk(slot->mu);
      if (slot->abandoned) {
        continue;
      }
      slot->result = std::move(r);
      slot->done = true;
      slot->cv.notify_one();
    }
    return batch.size();
  }

  // Teardown: fail every waiter so no worker thread blocks past destruction.
  void shutdown() {
    std::deque<std::shared_ptr<Slot>> batch;
    {
      std::lock_guard<std::mutex> lk(mu_);
      shutting_down_ = true;
      batch.swap(queue_);
    }
    for (auto& slot : batch) {
      std::lock_guard<std::mutex> lk(slot->mu);
      if (slot->abandoned) {
        continue;
      }
      slot->result = ToolResult::failure("assistant is shutting down");
      slot->done = true;
      slot->cv.notify_one();
    }
  }

 private:
  struct Slot {
    std::string name;
    nlohmann::json args;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    ToolResult result;
  };

  std::chrono::milliseconds timeout_;
  mutable std::mutex mu_;
  std::deque<std::shared_ptr<Slot>> queue_;
  bool shutting_down_ = false;
};

}  // namespace assistant_agent
