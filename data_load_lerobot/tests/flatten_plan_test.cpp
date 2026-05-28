#include "flatten_plan.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace lerobot;  // NOLINT(build/namespaces) — test-local convenience

TEST(FlattenedFieldNames, UsesInfoJsonNamesWhenAvailable) {
  const auto out = flattenedFieldNames("observation.state", 3, {"shoulder_pan", "elbow_flex", "wrist"});
  EXPECT_EQ(
      out, (std::vector<std::string>{
               "observation.state.shoulder_pan", "observation.state.elbow_flex", "observation.state.wrist"}));
}

TEST(FlattenedFieldNames, FallsBackToIndexWhenNamesMissingOrShort) {
  const auto out = flattenedFieldNames("action", 3, {"only_one"});
  EXPECT_EQ(out, (std::vector<std::string>{"action.only_one", "action.1", "action.2"}));
}

TEST(FlattenedFieldNames, EmptyForNonPositiveK) {
  EXPECT_TRUE(flattenedFieldNames("x", 0, {}).empty());
  EXPECT_TRUE(flattenedFieldNames("x", -2, {}).empty());
}

TEST(DedupeFieldNames, SuffixesDuplicatesStably) {
  const auto out = dedupeFieldNames({"a", "b", "a", "a", "b"});
  EXPECT_EQ(out, (std::vector<std::string>{"a", "b", "a__2", "a__3", "b__2"}));
}

TEST(DedupeFieldNames, NoOpWhenUnique) {
  const auto out = dedupeFieldNames({"x", "y", "z"});
  EXPECT_EQ(out, (std::vector<std::string>{"x", "y", "z"}));
}
