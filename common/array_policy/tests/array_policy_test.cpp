// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "pj_array_policy/array_policy.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using pj::array_policy::ArrayLimit;
using pj::array_policy::arrayLimitFromJson;
using pj::array_policy::arrayLimitToJson;
using pj::array_policy::ArrayPolicy;

TEST(ArrayLimitTest, DefaultsAreClamp500) {
  ArrayLimit limit;
  EXPECT_EQ(limit.max_size, 500u);
  EXPECT_EQ(limit.policy, ArrayPolicy::kClamp);
  EXPECT_TRUE(limit.clamp());
}

TEST(ArrayLimitTest, EmptyOrNonObjectConfigYieldsDefaults) {
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json::object()).max_size, 500u);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json::object()).policy, ArrayPolicy::kClamp);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json::array()).max_size, 500u);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json()).policy, ArrayPolicy::kClamp);
}

TEST(ArrayLimitTest, CanonicalKeysAreRead) {
  auto cfg = nlohmann::json{{"max_array_size", 32}, {"array_policy", "skip"}};
  auto limit = arrayLimitFromJson(cfg);
  EXPECT_EQ(limit.max_size, 32u);
  EXPECT_EQ(limit.policy, ArrayPolicy::kSkip);

  cfg["array_policy"] = "clamp";
  EXPECT_EQ(arrayLimitFromJson(cfg).policy, ArrayPolicy::kClamp);
}

TEST(ArrayLimitTest, ZeroMeansUnlimited) {
  auto cfg = nlohmann::json{{"max_array_size", 0}};
  EXPECT_EQ(arrayLimitFromJson(cfg).max_size, 0u);
}

TEST(ArrayLimitTest, LegacyDiscardKeyMapsToSkip) {
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"discard_large_arrays", true}}).policy, ArrayPolicy::kSkip);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"discard_large_arrays", false}}).policy, ArrayPolicy::kClamp);
}

TEST(ArrayLimitTest, LegacyClampKeyMapsToPolicy) {
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"clamp_large_arrays", true}}).policy, ArrayPolicy::kClamp);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"clamp_large_arrays", false}}).policy, ArrayPolicy::kSkip);
}

TEST(ArrayLimitTest, CanonicalPolicyKeyWinsOverLegacy) {
  auto cfg = nlohmann::json{{"array_policy", "skip"}, {"clamp_large_arrays", true}};
  EXPECT_EQ(arrayLimitFromJson(cfg).policy, ArrayPolicy::kSkip);
}

TEST(ArrayLimitTest, RoundTripThroughJson) {
  ArrayLimit original{.max_size = 128, .policy = ArrayPolicy::kSkip};
  nlohmann::json cfg;
  arrayLimitToJson(cfg, original);

  auto restored = arrayLimitFromJson(cfg);
  EXPECT_EQ(restored.max_size, original.max_size);
  EXPECT_EQ(restored.policy, original.policy);

  EXPECT_FALSE(cfg.value("clamp_large_arrays", true));
  EXPECT_TRUE(cfg.value("discard_large_arrays", false));
}
