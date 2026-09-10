// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the Lua-backed metadata query Engine (query_engine.h): eval,
// validate, metadata globals lifecycle. The query LANGUAGE itself (lexer,
// parser, completion, cursor analysis) is tested in common/query/tests/.
// Qt-, Arrow- and Flight-free; needs lua + sol2 only.

#include "query_engine.h"

#include <string_view>

#include "gtest/gtest.h"

namespace {

using mosaico::Engine;
using PJ::query::Metadata;
using PJ::query::Query;

TEST(Engine, EvalStringEqualityTruthy) {
  Engine e;
  e.set(Metadata{{"robot", "bonirob"}});
  EXPECT_TRUE(e.eval(Query("robot == \"bonirob\"")));
  EXPECT_FALSE(e.eval(Query("robot == \"other\"")));
}

TEST(Engine, EvalNumericComparison) {
  Engine e;
  // set() turns "42" into a Lua number, so > comparison works.
  e.set(Metadata{{"count", "42"}});
  EXPECT_TRUE(e.eval(Query("count > 40")));
  EXPECT_FALSE(e.eval(Query("count > 50")));
  EXPECT_TRUE(e.eval(Query("count == 42")));
}

TEST(Engine, EvalStringViewWithShorthand) {
  Engine e;
  e.set(Metadata{{"robot", "drone"}});
  // eval(string_view) parses + expands shorthand.
  EXPECT_TRUE(e.eval(std::string_view{"robot == \"humanoid\" or \"drone\""}));
  EXPECT_FALSE(e.eval(std::string_view{"robot == \"humanoid\" or \"tank\""}));
}

TEST(Engine, EvalRawLuaFallback) {
  Engine e;
  e.set(Metadata{{"name", "camera_front"}});
  // string.find isn't parseable by the subset parser → raw-Lua fallback.
  EXPECT_TRUE(e.eval(std::string_view{"string.find(name, \"camera\") ~= nil"}));
  EXPECT_FALSE(e.eval(std::string_view{"string.find(name, \"laser\") ~= nil"}));
}

TEST(Engine, EvalEmptyIsFalse) {
  Engine e;
  EXPECT_FALSE(e.eval(std::string_view{""}));
  EXPECT_FALSE(e.eval(Query("")));
}

TEST(Engine, EvalGarbageReturnsFalseNoThrow) {
  Engine e;
  e.set(Metadata{{"robot", "x"}});
  // Undefined global → comparing nil with string raises a Lua error →
  // caught → false. (Note: missing key compares nil.)
  EXPECT_FALSE(e.eval(Query("undefined_key == \"x\"")));
  // Pure syntax garbage.
  EXPECT_FALSE(e.eval(std::string_view{")))"}));
}

TEST(Engine, ClearRemovesGlobals) {
  Engine e;
  Metadata md{{"robot", "bonirob"}};
  e.set(md);
  EXPECT_TRUE(e.eval(Query("robot == \"bonirob\"")));
  e.clear(md);
  // After clear, robot is nil → comparison errors → false.
  EXPECT_FALSE(e.eval(Query("robot == \"bonirob\"")));
}

TEST(Engine, ValidateGoodSyntax) {
  auto r = Engine::validateText("robot == \"bonirob\"");
  EXPECT_TRUE(r.valid);
  EXPECT_TRUE(r.error.empty());
}

TEST(Engine, ValidateEmptyIsInvalid) {
  auto r = Engine::validateText("");
  EXPECT_FALSE(r.valid);
  EXPECT_EQ(r.error, "empty query");
}

TEST(Engine, ValidateSyntaxErrorReportsLine) {
  // ")))" expands to raw, wrapped "return ()))" → Lua syntax error.
  auto r = Engine::validateText(")))");
  EXPECT_FALSE(r.valid);
  EXPECT_FALSE(r.error.empty());
  // parse_error extracts a line number from the "...:N:..." message.
  EXPECT_GE(r.line, 1);
}

TEST(Engine, ValidatePartialIsValidLuaButFalsy) {
  // "robot ==" is incomplete for the parser, falls back to raw source
  // "robot ==", wrapped "return (robot ==)" → Lua SYNTAX ERROR → invalid.
  auto r = Engine::validateText("robot ==");
  EXPECT_FALSE(r.valid);
}

TEST(Engine, ThreadLocalReuseAcrossValidateCalls) {
  // validate() reuses a thread_local sol::state. Repeated calls must be
  // independent (a prior failed load must not corrupt the next).
  EXPECT_FALSE(Engine::validateText(")))").valid);
  EXPECT_TRUE(Engine::validateText("robot == \"x\"").valid);
  EXPECT_FALSE(Engine::validateText("=== bad").valid);
  EXPECT_TRUE(Engine::validateText("a == 1 and b == 2").valid);
}

}  // namespace
