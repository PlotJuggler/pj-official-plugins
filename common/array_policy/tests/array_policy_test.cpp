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

TEST(ArrayLimitTest, ToJsonMirrorsClampCaseForLegacyReaders) {
  // An old plugin .so reads only the legacy bools; make sure a Clamp limit
  // serializes them both consistently (clamp=true, discard=false).
  nlohmann::json cfg;
  arrayLimitToJson(cfg, ArrayLimit{.max_size = 500, .policy = ArrayPolicy::kClamp});
  EXPECT_TRUE(cfg.value("clamp_large_arrays", false));
  EXPECT_FALSE(cfg.value("discard_large_arrays", true));
  EXPECT_EQ(cfg.value("array_policy", std::string{}), "clamp");
}

// --- Robustness: honor the "never throws, fall back to the default for a
// malformed field" contract documented on arrayLimitFromJson. A config may be
// hand-edited or written by a third-party/older plugin, so a wrong-typed or
// out-of-range field must degrade to the default, never throw and never wrap.

TEST(ArrayLimitTest, WrongTypeMaxArraySizeFallsBackToDefault) {
  for (const auto& bad :
       {nlohmann::json("500"), nlohmann::json(nullptr), nlohmann::json::array({1, 2}),
        nlohmann::json::object({{"k", 1}}), nlohmann::json(true), nlohmann::json(3.7)}) {
    auto cfg = nlohmann::json::object();
    cfg["max_array_size"] = bad;
    ArrayLimit limit;
    EXPECT_NO_THROW(limit = arrayLimitFromJson(cfg)) << "input: " << bad.dump();
    EXPECT_EQ(limit.max_size, 500u) << "input: " << bad.dump();
  }
}

TEST(ArrayLimitTest, NegativeMaxArraySizeFallsBackToDefault) {
  EXPECT_NO_THROW((void)arrayLimitFromJson(nlohmann::json{{"max_array_size", -1}}));
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"max_array_size", -1}}).max_size, 500u);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"max_array_size", -500}}).max_size, 500u);
}

TEST(ArrayLimitTest, OutOfRangeMaxArraySizeFallsBackToDefault) {
  // > UINT32_MAX must not be truncated modulo 2^32.
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"max_array_size", 5000000000LL}}).max_size, 500u);
}

TEST(ArrayLimitTest, WrongTypeLegacyBoolsFallBackToDefault) {
  // Non-bool legacy keys must not throw and must leave the policy at default.
  EXPECT_NO_THROW((void)arrayLimitFromJson(nlohmann::json{{"discard_large_arrays", "true"}}));
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"discard_large_arrays", "true"}}).policy, ArrayPolicy::kClamp);
  EXPECT_EQ(arrayLimitFromJson(nlohmann::json{{"clamp_large_arrays", 1}}).policy, ArrayPolicy::kClamp);
}
