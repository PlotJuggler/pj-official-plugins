// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "model_picker.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using assistant_agent::ModelChoice;
using assistant_agent::ModelPicker;

std::vector<ModelChoice> sampleChoices() {
  return {
      {"sonnet", "sonnet — fast; the measured default"},
      {"opus", "opus"},
      {"haiku", "haiku"},
  };
}

TEST(ModelPicker, ItemsPrependsCliDefaultAndCustomBeforeTheListedLabels) {
  const ModelPicker picker(sampleChoices());
  const std::vector<std::string> items = picker.items();
  ASSERT_EQ(items.size(), 5u);
  EXPECT_EQ(items[0], "CLI default");
  EXPECT_EQ(items[1], "Custom...");
  EXPECT_EQ(items[2], "sonnet — fast; the measured default");
  EXPECT_EQ(items[3], "opus");
  EXPECT_EQ(items[4], "haiku");
}

TEST(ModelPicker, EmptyChoicesStillOffersTheTwoFixedRows) {
  const ModelPicker picker({});
  EXPECT_EQ(picker.items(), (std::vector<std::string>{"CLI default", "Custom..."}));
}

TEST(ModelPicker, IndexForValueMapsEmptyToCliDefault) {
  const ModelPicker picker(sampleChoices());
  EXPECT_EQ(picker.indexForValue(""), ModelPicker::kIndexDefault);
}

TEST(ModelPicker, IndexForValueMapsAListedIdToItsOwnRow) {
  const ModelPicker picker(sampleChoices());
  EXPECT_EQ(picker.indexForValue("sonnet"), ModelPicker::kIndexFirstListed + 0);
  EXPECT_EQ(picker.indexForValue("opus"), ModelPicker::kIndexFirstListed + 1);
  EXPECT_EQ(picker.indexForValue("haiku"), ModelPicker::kIndexFirstListed + 2);
}

TEST(ModelPicker, IndexForValueMapsAnUnlistedIdToCustom) {
  const ModelPicker picker(sampleChoices());
  EXPECT_EQ(picker.indexForValue("claude-3-7-unreleased"), ModelPicker::kIndexCustom);
}

TEST(ModelPicker, ValueForIndexMapsCliDefaultToEmptyString) {
  const ModelPicker picker(sampleChoices());
  const std::optional<std::string> value = picker.valueForIndex(ModelPicker::kIndexDefault);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "");
}

TEST(ModelPicker, ValueForIndexMapsAListedRowToItsId) {
  const ModelPicker picker(sampleChoices());
  const std::optional<std::string> value = picker.valueForIndex(ModelPicker::kIndexFirstListed + 1);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "opus");
}

TEST(ModelPicker, ValueForIndexMapsCustomToNulloptSoTheCallerKeepsTheTypedText) {
  const ModelPicker picker(sampleChoices());
  EXPECT_FALSE(picker.valueForIndex(ModelPicker::kIndexCustom).has_value());
}

TEST(ModelPicker, ValueForIndexMapsAnOutOfRangeListedRowToNullopt) {
  const ModelPicker picker(sampleChoices());
  // Only 3 choices; kIndexFirstListed + 3 is one past the last one.
  EXPECT_FALSE(picker.valueForIndex(ModelPicker::kIndexFirstListed + 3).has_value());
  EXPECT_FALSE(picker.valueForIndex(-1).has_value());
}

TEST(ModelPicker, RoundTripsEveryListedChoiceThroughBothDirections) {
  const std::vector<ModelChoice> choices = sampleChoices();
  const ModelPicker picker(choices);
  for (const ModelChoice& c : choices) {
    const int index = picker.indexForValue(c.id);
    const std::optional<std::string> back = picker.valueForIndex(index);
    ASSERT_TRUE(back.has_value()) << c.id;
    EXPECT_EQ(*back, c.id);
  }
}

}  // namespace
