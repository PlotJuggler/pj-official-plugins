// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// tests/fixtures/codex_models_cache.json is a verbatim copy of a real
// ~/.codex/models_cache.json (a public catalog Codex itself downloads and
// caches) -- nothing here reads or writes the real file.
#include "codex_models.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/scoped_env.hpp"

#ifndef ASSISTANT_CODEX_MODELS_FIXTURE
#error "ASSISTANT_CODEX_MODELS_FIXTURE must be defined by CMake"
#endif

namespace {

using assistant_agent::codexModelsCacheFile;
using assistant_agent::listCodexModels;
using assistant_agent::ModelChoice;
using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

TEST(ListCodexModels, KeepsOnlyListVisibilitySortedByPriorityWithComposedLabels) {
  const std::vector<ModelChoice> models = listCodexModels(ASSISTANT_CODEX_MODELS_FIXTURE);

  // gpt-reserve (priority 3, "hide") and codex-auto-review (priority 43,
  // "hide") must both be absent -- neither is meant for a human to pick.
  std::vector<std::string> ids;
  ids.reserve(models.size());
  for (const auto& m : models) {
    ids.push_back(m.id);
  }
  ASSERT_EQ(ids.size(), 4u) << "hidden entries must be filtered out";
  EXPECT_EQ(ids, (std::vector<std::string>{"gpt-5.6-terra", "gpt-5.6-luna", "gpt-5.5", "gpt-5.4-mini"}))
      << "must be sorted by ascending priority (7, 8, 12, 23), not file order";

  EXPECT_EQ(models[0].label, "GPT-5.6-Terra — Balanced agentic coding model for everyday work.");
  EXPECT_EQ(models[1].label, "GPT-5.6-Luna — Fast and affordable agentic coding model.");
  EXPECT_EQ(models[2].label, "GPT-5.5 — Proven previous-generation model for coding and general work.");
  EXPECT_EQ(models[3].label, "GPT-5.4-Mini — Small, fast, and cost-efficient model for simpler coding tasks.");
}

TEST(ListCodexModels, MissingFileYieldsEmptyNotThrow) {
  EXPECT_TRUE(listCodexModels(std::filesystem::path("/nonexistent/pj-assistant-test/models_cache.json")).empty());
  EXPECT_TRUE(listCodexModels(std::filesystem::path{}).empty());
}

TEST(ListCodexModels, CorruptFileYieldsEmptyNotThrow) {
  const std::filesystem::path dir = makeTempDir("assistant_codex_models_test_");
  ASSERT_FALSE(dir.empty());
  const std::filesystem::path corrupt = dir / "models_cache.json";
  {
    std::ofstream f(corrupt);
    f << "{ this is not valid json";
  }
  EXPECT_TRUE(listCodexModels(corrupt).empty());

  const std::filesystem::path wrong_shape = dir / "wrong_shape.json";
  {
    std::ofstream f(wrong_shape);
    f << R"({"fetched_at": "now", "models": "not-an-array"})";
  }
  EXPECT_TRUE(listCodexModels(wrong_shape).empty());

  const std::filesystem::path not_object = dir / "not_object.json";
  {
    std::ofstream f(not_object);
    f << "[1, 2, 3]";
  }
  EXPECT_TRUE(listCodexModels(not_object).empty());

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(CodexModelsCacheFile, ResolvesUnderCodexHomeThenHome) {
  {
    ScopedEnv codex_home("CODEX_HOME", "/tmp/pj-assistant-test-codex-home");
    EXPECT_EQ(codexModelsCacheFile(), std::filesystem::path("/tmp/pj-assistant-test-codex-home/models_cache.json"));
  }
  {
    ScopedEnv codex_home("CODEX_HOME", nullptr);
    ScopedEnv home("HOME", "/tmp/pj-assistant-test-home");
    EXPECT_EQ(codexModelsCacheFile(), std::filesystem::path("/tmp/pj-assistant-test-home/.codex/models_cache.json"));
  }
  {
    ScopedEnv codex_home("CODEX_HOME", nullptr);
    ScopedEnv home("HOME", nullptr);
    EXPECT_TRUE(codexModelsCacheFile().empty());
  }
}

}  // namespace
