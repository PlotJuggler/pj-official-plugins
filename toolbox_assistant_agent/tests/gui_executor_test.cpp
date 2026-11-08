// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// GuiExecutor: pending evaluations are released on the GUI thread on shutdown, abandonment and
// cancel, and resumed across ticks without resubmitting. Driven against the SDK's ToolRegistry and
// fake hosts.
#include "gui_executor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <nlohmann/json.hpp>
#include <pj_plugins/testing/fake_catalog_host.hpp>
#include <pj_plugins/testing/fake_playback_viewport_hosts.hpp>
#include <pj_plugins/testing/recording_dp_host.hpp>
#include <thread>

#include "pj_base/agent_tools/tool_registry.hpp"

namespace {

using PJ::sdk::agent_tools::ToolContext;
using PJ::sdk::agent_tools::ToolRegistry;
using PJ::testing::FakeCatalogHost;
using PJ::testing::FakePlaybackHost;
using PJ::testing::RecordingDpHost;

constexpr std::int64_t kSec = 1'000'000'000;

TEST(GuiExecutor, ShutdownReleasesPendingEvaluationOnGuiThread) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.pending_polls = 100;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  assistant_agent::GuiExecutor executor;
  auto worker = std::async(std::launch::async, [&] {
    return executor.call(
        "evaluate",
        {{"inputs", {"/cloud"}}, {"at_s", 1.0}, {"body", "return {count=1}"}, {"outputs", {"count:number"}}});
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (executor.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(executor.empty());
  executor.drain(reg, ctx);
  EXPECT_EQ(dp.poll_calls, 1);
  EXPECT_EQ(worker.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  executor.shutdown();
  EXPECT_FALSE(worker.get().ok);
  EXPECT_EQ(dp.released_handles.size(), 1u);
}

TEST(GuiExecutor, AbandonedContinuationReleasesAtNextGuiTick) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.pending_polls = 100;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  assistant_agent::GuiExecutor executor(std::chrono::milliseconds(500));
  auto worker = std::async(std::launch::async, [&] {
    return executor.call(
        "evaluate",
        {{"inputs", {"/cloud"}}, {"at_s", 1.0}, {"body", "return {count=1}"}, {"outputs", {"count:number"}}});
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (executor.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  executor.drain(reg, ctx);
  EXPECT_EQ(dp.poll_calls, 1);
  EXPECT_FALSE(worker.get().ok);
  EXPECT_TRUE(dp.released_handles.empty());
  executor.drain(reg, ctx);
  EXPECT_EQ(dp.poll_calls, 1);
  EXPECT_EQ(dp.released_handles.size(), 1u);
}

TEST(GuiExecutor, ResumesPendingEvaluationWithoutResubmitting) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.pending_polls = 2;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  assistant_agent::GuiExecutor executor;
  auto worker = std::async(std::launch::async, [&] {
    return executor.call(
        "evaluate",
        {{"inputs", {"/cloud"}}, {"at_s", 1.0}, {"body", "return {count=1}"}, {"outputs", {"count:number"}}});
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (executor.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  for (int tick = 0; tick < 3; ++tick) {
    executor.drain(reg, ctx);
  }
  EXPECT_TRUE(worker.get().ok);
  EXPECT_EQ(dp.submit_calls, 1);
  EXPECT_EQ(dp.poll_calls, 3);
  EXPECT_EQ(dp.released_handles.size(), 1u);
}

TEST(GuiExecutor, CancelReleasesPendingEvaluationAndAllowsAnotherTurn) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.pending_polls = 100;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  assistant_agent::GuiExecutor executor;
  auto worker = std::async(std::launch::async, [&] {
    return executor.call(
        "evaluate",
        {{"inputs", {"/cloud"}}, {"at_s", 1.0}, {"body", "return {count=1}"}, {"outputs", {"count:number"}}});
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (executor.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(executor.empty());
  executor.drain(reg, ctx);
  EXPECT_EQ(dp.poll_calls, 1);
  EXPECT_EQ(worker.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  executor.cancelPending();
  EXPECT_FALSE(worker.get().ok);
  EXPECT_EQ(dp.released_handles.size(), 1u);
  auto next = std::async(std::launch::async, [&] { return executor.call("report_status", {}); });
  const auto next_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (executor.empty() && std::chrono::steady_clock::now() < next_deadline) {
    std::this_thread::yield();
  }
  executor.drain(reg, ctx);
  EXPECT_TRUE(next.get().ok);
}

}  // namespace
