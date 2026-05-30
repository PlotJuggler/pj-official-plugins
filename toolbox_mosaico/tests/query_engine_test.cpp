// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Comprehensive unit tests for the ported PJ3 metadata-query engine
// (src/query/*.h + src/core/types.h). These exercise the Lexer, Parser,
// serializer, Query facade, the cursor-context completion engine
// (complete()/analyze()), and the Lua-backed Engine. They are Qt-, Arrow-,
// and Flight-independent; they need only Lua + sol2 + GTest.
//
// The engine headers are byte-for-byte identical to the PJ3 reference, so
// every assertion below pins the EXISTING behavior described in the header
// comments. A failure here is a regression, not a spec change.

#include "gtest/gtest.h"
#include "query/ast.h"
#include "query/complete.h"
#include "query/edit.h"
#include "query/engine.h"
#include "query/query.h"
#include "query/token.h"
#include "types.h"

namespace {

// A small fixed schema used across completion/analyze tests.
Schema makeSchema() {
  return Schema{
      {"robot", {"bonirob", "other"}},
      {"sensor", {"camera", "laser"}},
  };
}

// ---------------------------------------------------------------------------
// Lexer / tokenize (token.h)
// ---------------------------------------------------------------------------

TEST(Lexer, TwoCharOperators) {
  auto toks = Lexer("a == b ~= c <= d >= e").tokenize();
  // a Op b Op c Op d Op e
  ASSERT_EQ(toks.size(), 9u);
  EXPECT_EQ(toks[1].type, TokenType::Operator);
  EXPECT_EQ(toks[1].text, "==");
  EXPECT_EQ(toks[3].text, "~=");
  EXPECT_EQ(toks[5].text, "<=");
  EXPECT_EQ(toks[7].text, ">=");
}

TEST(Lexer, SingleCharComparisonOperators) {
  auto toks = Lexer("a < b > c").tokenize();
  ASSERT_EQ(toks.size(), 5u);
  EXPECT_EQ(toks[1].type, TokenType::Operator);
  EXPECT_EQ(toks[1].text, "<");
  EXPECT_EQ(toks[3].type, TokenType::Operator);
  EXPECT_EQ(toks[3].text, ">");
}

TEST(Lexer, LoneEqualsAndTildeAreOperators) {
  // token.h: "Lone = or ~ (not part of a two-char op)" → Operator.
  auto toks = Lexer("a = b").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[1].type, TokenType::Operator);
  EXPECT_EQ(toks[1].text, "=");

  auto toks2 = Lexer("a ~ b").tokenize();
  ASSERT_EQ(toks2.size(), 3u);
  EXPECT_EQ(toks2[1].type, TokenType::Operator);
  EXPECT_EQ(toks2[1].text, "~");
}

TEST(Lexer, QuotedStringsWithEscapes) {
  // Double and single quotes are Values; escaped quote does not terminate.
  auto toks = Lexer(R"(robot == "boni\"rob")").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::Value);
  EXPECT_EQ(toks[2].text, R"("boni\"rob")");

  auto toks2 = Lexer("k == 'val'").tokenize();
  ASSERT_EQ(toks2.size(), 3u);
  EXPECT_EQ(toks2[2].type, TokenType::Value);
  EXPECT_EQ(toks2[2].text, "'val'");
}

TEST(Lexer, UnterminatedQuoteConsumesToEnd) {
  auto toks = Lexer("k == \"unterminated").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::Value);
  EXPECT_EQ(toks[2].text, "\"unterminated");
}

TEST(Lexer, NumericLiteralsClassifyAsValue) {
  // classify_word: strtod consumes the whole word → Value.
  auto toks = Lexer("count >= 42").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::Value);
  EXPECT_EQ(toks[2].text, "42");

  auto toks2 = Lexer("x == 3.14").tokenize();
  EXPECT_EQ(toks2[2].type, TokenType::Value);

  auto toks3 = Lexer("x == -1.5e3").tokenize();
  EXPECT_EQ(toks3[2].type, TokenType::Value);
}

TEST(Lexer, NonNumericWordsClassifyAsKey) {
  auto toks = Lexer("robot_type").tokenize();
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].type, TokenType::Key);
  EXPECT_EQ(toks[0].text, "robot_type");

  // Word that starts numeric but isn't fully numeric → Key.
  auto toks2 = Lexer("3abc").tokenize();
  ASSERT_EQ(toks2.size(), 1u);
  EXPECT_EQ(toks2[0].type, TokenType::Key);
}

TEST(Lexer, KeywordsAndOrNot) {
  // "a and b or not c" → a, and, b, or, not, c = 6 tokens.
  auto toks = Lexer("a and b or not c").tokenize();
  ASSERT_EQ(toks.size(), 6u);
  EXPECT_EQ(toks[1].type, TokenType::And);
  EXPECT_EQ(toks[3].type, TokenType::Or);
  EXPECT_EQ(toks[4].type, TokenType::Not);
}

TEST(Lexer, Parens) {
  auto toks = Lexer("( a )").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[0].type, TokenType::OpenParen);
  EXPECT_EQ(toks[0].text, "(");
  EXPECT_EQ(toks[2].type, TokenType::CloseParen);
  EXPECT_EQ(toks[2].text, ")");
}

TEST(Lexer, TokenOffsets) {
  // "robot == \"x\"": robot[0,5) ==[6,8) "x"[9,12)
  auto toks = Lexer("robot == \"x\"").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[0].start, 0);
  EXPECT_EQ(toks[0].end, 5);
  EXPECT_EQ(toks[1].start, 6);
  EXPECT_EQ(toks[1].end, 8);
  EXPECT_EQ(toks[2].start, 9);
  EXPECT_EQ(toks[2].end, 12);
}

TEST(Lexer, WhitespaceSkippedAndEmptyInput) {
  EXPECT_TRUE(Lexer("    ").tokenize().empty());
  EXPECT_TRUE(Lexer("").tokenize().empty());
  EXPECT_TRUE(Lexer("\t\n ").tokenize().empty());
}

TEST(Lexer, UnrecognizedCharsSkippedNoCrash) {
  // token.h promises unrecognized chars are skipped (no infinite loop).
  // Characters like ! , ; are not punct and not space, so they form a "word".
  // But a truly stray punctuation that the word loop won't consume must be
  // skipped. Use a char the is_punct set excludes but is also handled: none
  // exist, so confirm a control char between keys still yields both keys.
  auto toks = Lexer("a\x01 b").tokenize();
  // \x01 is not space and not punct, so it joins the "a" word; the word ends
  // at the space. So we get ["a\x01", "b"].
  ASSERT_EQ(toks.size(), 2u);
  EXPECT_EQ(toks[1].text, "b");
}

// ---------------------------------------------------------------------------
// Parser + serialize (ast.h)
// ---------------------------------------------------------------------------

TEST(Parser, FullClause) {
  auto toks = Lexer("robot == \"bonirob\"").tokenize();
  auto res = Parser(toks).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Compare);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(res.error.empty());
  EXPECT_EQ(serialize(res.ast.get()), "robot == \"bonirob\"");
}

TEST(Parser, BareKeyIsKeyExprAndIncomplete) {
  auto res = Parser(Lexer("robot").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Key);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot");
}

TEST(Parser, PartialKeyOpIsPartialExprAndIncomplete) {
  auto res = Parser(Lexer("robot ==").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Partial);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot ==");
}

TEST(Parser, AndOrPrecedence) {
  // "or has lower precedence than and": A or B and C → A or (B and C)
  auto res = Parser(Lexer("a == \"1\" or b == \"2\" and c == \"3\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Binary);
  auto* top = static_cast<const BinaryExpr*>(res.ast.get());
  EXPECT_EQ(top->connective.type, TokenType::Or);
  // Right side is the "and" subtree.
  ASSERT_EQ(top->right->type, NodeType::Binary);
  EXPECT_EQ(static_cast<const BinaryExpr*>(top->right.get())->connective.type, TokenType::And);
  EXPECT_TRUE(res.complete);
}

TEST(Parser, NotExpr) {
  auto res = Parser(Lexer("not robot == \"x\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Not);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "not robot == \"x\"");
}

TEST(Parser, NestedGroup) {
  auto res = Parser(Lexer("(a == \"1\" or b == \"2\")").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Group);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "(a == \"1\" or b == \"2\")");
}

TEST(Parser, EmptyParensYieldsNullAst) {
  // "()" — parse_primary consumes "(", recurses into parse_or which reaches
  // the ")"; none of its branches match it, so the "Unexpected token" path
  // skips (consumes) the ")" and returns null. Back in the OpenParen branch
  // the close-paren is already gone, so the CloseParen check is false and the
  // (null) inner is returned. Net: empty parens produce a null AST, marked
  // incomplete with no error.
  auto res = Parser(Lexer("()").tokenize()).parse();
  EXPECT_EQ(res.ast, nullptr);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "");
}

TEST(Parser, UnclosedParenReturnsInner) {
  // parse_primary: unclosed paren returns inner.
  auto res = Parser(Lexer("(a == \"1\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Compare);
  EXPECT_TRUE(res.complete);  // the inner compare is complete, no leftover tokens
}

TEST(Parser, ShorthandExpansion) {
  // ast.h header: robot == "a" or "b" → robot == "a" or robot == "b"
  auto res = Parser(Lexer("robot == \"a\" or \"b\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot == \"a\" or robot == \"b\"");
}

TEST(Parser, ShorthandExpansionWithAnd) {
  auto res = Parser(Lexer("robot == \"a\" and \"b\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot == \"a\" and robot == \"b\"");
}

TEST(Parser, EmptyInputIsIncompleteWithError) {
  auto res = Parser({}).parse();
  EXPECT_FALSE(res.ast);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(res.error, "empty");
}

TEST(Parser, LeftoverTokensMarkIncomplete) {
  // "a == \"1\" foo" — the leftover bare key "foo" is parsed in parse_or as a
  // separate clause? No: parse_or stops at a non-connective. So "foo" is
  // leftover → not complete, error "unexpected tokens".
  auto res = Parser(Lexer("a == \"1\" foo").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(res.error, "unexpected tokens");
}

TEST(Parser, DanglingConnectiveStopsCleanly) {
  // "a == \"1\" or" — parse_or consumes the "or" (pos_++) THEN parse_and
  // returns null and breaks. The "or" is already consumed, so there is no
  // leftover token: the result is the lone CompareExpr, which is complete.
  // (This is the engine's actual behavior — the trailing connective is simply
  // dropped, not flagged.)
  auto res = Parser(Lexer("a == \"1\" or").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::Compare);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(res.error.empty());
}

// ---------------------------------------------------------------------------
// Query facade (query.h)
// ---------------------------------------------------------------------------

TEST(Query, LuaExpandsCompleteQuery) {
  Query q("robot == \"a\" or \"b\"", {});
  EXPECT_TRUE(q.complete());
  EXPECT_EQ(q.lua(), "robot == \"a\" or robot == \"b\"");
}

TEST(Query, LuaFallsBackToRawForIncomplete) {
  // query.h: incomplete → lua_str_ = source_ (the raw source).
  Query q("robot ==", {});
  EXPECT_FALSE(q.complete());
  EXPECT_EQ(q.lua(), "robot ==");

  // Bare key — also incomplete, raw source.
  Query bare("robot", {});
  EXPECT_FALSE(bare.complete());
  EXPECT_EQ(bare.lua(), "robot");
}

TEST(Query, EmptyQuery) {
  Query q("", {});
  EXPECT_TRUE(q.empty());
  EXPECT_TRUE(q.lua().empty());
  EXPECT_EQ(q.ast(), nullptr);
}

TEST(Query, TokenAtAndIndexAt) {
  Query q("robot == \"x\"", {});
  // robot[0,5) ==[6,8) "x"[9,12)
  ASSERT_NE(q.token_at(0), nullptr);
  EXPECT_EQ(q.token_at(0)->text, "robot");
  EXPECT_EQ(q.token_index_at(0), 0);
  // pos 5 is the space — no token covers [5] since end is exclusive.
  EXPECT_EQ(q.token_at(5), nullptr);
  EXPECT_EQ(q.token_index_at(5), -1);
  EXPECT_EQ(q.token_at(6)->text, "==");
  EXPECT_EQ(q.token_index_at(6), 1);
  EXPECT_EQ(q.token_at(10)->text, "\"x\"");
  EXPECT_EQ(q.token_index_at(10), 2);
}

TEST(Query, KeyBefore) {
  Query q("robot == \"x\" and sensor ==", {});
  // Tokens: robot Op Val And sensor Op
  // key_before(index 5 = the trailing op) → "sensor".
  EXPECT_EQ(q.key_before(5), "sensor");
  // key_before(index 1 = first op) → "robot".
  EXPECT_EQ(q.key_before(1), "robot");
  // key_before(0) → no preceding key → empty.
  EXPECT_EQ(q.key_before(0), "");
}

TEST(Query, ExpectedAtTransitions) {
  Query q("robot == \"x\"", {});
  // Before any token → Key.
  EXPECT_EQ(q.expected_at(-1), TokenType::Key);
  // Cursor inside "robot" (pos 2) → editing a Key.
  EXPECT_EQ(q.expected_at(2), TokenType::Key);
  // After "robot" (pos 5, past end) → Operator next.
  EXPECT_EQ(q.expected_at(5), TokenType::Operator);
  // After "==" (pos 8) → Value next.
  EXPECT_EQ(q.expected_at(8), TokenType::Value);
  // After the value (pos 12) → connective (And).
  EXPECT_EQ(q.expected_at(12), TokenType::And);
}

TEST(Query, ExpectedAtAfterConnectiveAndParen) {
  Query q("a == \"1\" and ( b == \"2\" )", {});
  // After the "and" → Key.
  // "and" token is at some offset; pick a position right after it.
  const Token* and_tok = nullptr;
  for (const auto& t : q.tokens()) {
    if (t.type == TokenType::And) {
      and_tok = &t;
    }
  }
  ASSERT_NE(and_tok, nullptr);
  EXPECT_EQ(q.expected_at(and_tok->end), TokenType::Key);
}

// ---------------------------------------------------------------------------
// complete() (complete.h)
// ---------------------------------------------------------------------------

TEST(Complete, AtStartExpectsKeysAny) {
  auto schema = makeSchema();
  auto c = complete("", 0, schema);
  EXPECT_EQ(c.expect, Expect::Any);
  // Suggestions are all schema keys.
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterKnownKeyExpectsOperator) {
  auto schema = makeSchema();
  auto c = complete("robot", 5, schema);
  EXPECT_EQ(c.expect, Expect::Operator);
  EXPECT_EQ(c.current_key, "robot");
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"==", "~=", "<", ">", "<=", ">="}));
}

TEST(Complete, PartialKeyFiltersByPrefix) {
  auto schema = makeSchema();
  auto c = complete("rob", 3, schema);
  EXPECT_EQ(c.expect, Expect::Key);
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"robot"}));
}

TEST(Complete, AfterOperatorExpectsThatKeysValues) {
  auto schema = makeSchema();
  auto c = complete("robot ==", 8, schema);
  EXPECT_EQ(c.expect, Expect::Value);
  EXPECT_EQ(c.current_key, "robot");
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"bonirob", "other"}));

  auto c2 = complete("sensor ==", 9, schema);
  EXPECT_EQ(c2.current_key, "sensor");
  EXPECT_EQ(c2.suggestions, (std::vector<std::string>{"camera", "laser"}));
}

TEST(Complete, AfterValueExpectsConnective) {
  auto schema = makeSchema();
  // key op value → after the value, expect a connective (and/or).
  auto c = complete("robot == \"bonirob\"", 18, schema);
  EXPECT_EQ(c.expect, Expect::Connective);
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"and", "or"}));
}

TEST(Complete, AfterConnectiveExpectsKey) {
  auto schema = makeSchema();
  auto c = complete("robot == \"bonirob\" and", 22, schema);
  EXPECT_EQ(c.expect, Expect::Key);
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterOpenParenExpectsKey) {
  auto schema = makeSchema();
  auto c = complete("(", 1, schema);
  EXPECT_EQ(c.expect, Expect::Key);
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterCloseParenExpectsConnective) {
  auto schema = makeSchema();
  auto c = complete("(robot == \"x\")", 14, schema);
  EXPECT_EQ(c.expect, Expect::Connective);
  EXPECT_EQ(c.suggestions, (std::vector<std::string>{"and", "or"}));
}

TEST(Complete, ExpandShorthand) {
  EXPECT_EQ(expand("robot == \"a\" or \"b\""), "robot == \"a\" or robot == \"b\"");
  EXPECT_EQ(expand("robot == \"a\" and \"b\""), "robot == \"a\" and robot == \"b\"");
  // Fewer than 4 tokens → returned verbatim.
  EXPECT_EQ(expand("robot == \"a\""), "robot == \"a\"");
}

// ---------------------------------------------------------------------------
// analyze() + find_active_token (edit.h)
// ---------------------------------------------------------------------------

TEST(FindActiveToken, BoundaryAtTokenEnd) {
  // edit.h: cursor at P is "on" a token if P in [start, end] (inclusive end).
  auto toks = Lexer("robot == \"x\"").tokenize();
  // robot[0,5): cursor at 5 (== end) still "on" robot.
  EXPECT_EQ(find_active_token(toks, 5), 0);
  // cursor at 0 (== start) on robot.
  EXPECT_EQ(find_active_token(toks, 0), 0);
  // cursor at 6 (== start of "==") on the operator.
  EXPECT_EQ(find_active_token(toks, 6), 1);
}

TEST(Analyze, AtStartExpectsAnyKeyInsert) {
  auto schema = makeSchema();
  auto ctx = analyze("", 0, schema);
  EXPECT_EQ(ctx.token_index, -1);
  EXPECT_EQ(ctx.expect, Expect::Any);
  // Key dropdown inserts at Any; op/val disabled.
  EXPECT_EQ(ctx.key_action, Action::Insert);
  EXPECT_EQ(ctx.op_action, Action::Disabled);
  EXPECT_EQ(ctx.val_action, Action::Disabled);
  EXPECT_TRUE(ctx.can_pick_key());
  EXPECT_FALSE(ctx.can_pick_op());
  EXPECT_FALSE(ctx.can_pick_value());
}

TEST(Analyze, CursorOnKeyReplacesKeyInsertsOp) {
  auto schema = makeSchema();
  // Cursor at end of "robot" (pos 5) — on the Key token.
  auto ctx = analyze("robot", 5, schema);
  ASSERT_EQ(ctx.token_index, 0);
  EXPECT_EQ(ctx.active_token.type, TokenType::Key);
  // edit.h: on a Key → expect = Operator, context_key = the key.
  EXPECT_EQ(ctx.expect, Expect::Operator);
  EXPECT_EQ(ctx.context_key, "robot");
  // Key dropdown Replaces (cursor on key); Op dropdown Inserts (op next).
  EXPECT_EQ(ctx.key_action, Action::Replace);
  EXPECT_EQ(ctx.op_action, Action::Insert);
  EXPECT_EQ(ctx.val_action, Action::Disabled);
}

TEST(Analyze, CursorOnOperatorReplacesOpInsertsValue) {
  auto schema = makeSchema();
  // "robot ==" — cursor at end (pos 8) on the operator.
  auto ctx = analyze("robot ==", 8, schema);
  ASSERT_EQ(ctx.token_index, 1);
  EXPECT_EQ(ctx.active_token.type, TokenType::Operator);
  EXPECT_EQ(ctx.expect, Expect::Value);
  EXPECT_EQ(ctx.context_key, "robot");  // key before the operator
  EXPECT_EQ(ctx.op_action, Action::Replace);
  EXPECT_EQ(ctx.val_action, Action::Insert);
}

TEST(Analyze, CursorOnValueReplacesValueExpectConnective) {
  auto schema = makeSchema();
  // "robot == \"x\"" cursor at end (pos 12) on the Value token.
  auto ctx = analyze("robot == \"x\"", 12, schema);
  ASSERT_EQ(ctx.token_index, 2);
  EXPECT_EQ(ctx.active_token.type, TokenType::Value);
  EXPECT_EQ(ctx.expect, Expect::Connective);
  EXPECT_EQ(ctx.context_key, "robot");
  EXPECT_EQ(ctx.val_action, Action::Replace);
  // Connective → key dropdown can auto-chain (Insert).
  EXPECT_EQ(ctx.key_action, Action::Insert);
}

TEST(Analyze, ReplaceRangeMatchesActiveToken) {
  auto schema = makeSchema();
  auto ctx = analyze("robot", 3, schema);  // cursor inside "robot"
  ASSERT_EQ(ctx.token_index, 0);
  // The replacement range is the active token's [start, end).
  EXPECT_EQ(ctx.active_token.start, 0);
  EXPECT_EQ(ctx.active_token.end, 5);
  EXPECT_EQ(ctx.key_action, Action::Replace);
}

TEST(Analyze, WhitespaceBetweenTokensNoActiveToken) {
  auto schema = makeSchema();
  // "robot ==" the space at pos 5? find_active_token: 5 is end of robot →
  // still on robot. Use "robot  ==" with cursor at the gap (pos 6, between
  // two spaces) where no token covers it.
  auto ctx = analyze("robot  ==", 6, schema);
  EXPECT_EQ(ctx.token_index, -1);
}

// ---------------------------------------------------------------------------
// Engine (engine.h)
// ---------------------------------------------------------------------------

TEST(Engine, EvalStringEqualityTruthy) {
  Engine e;
  e.set(Metadata{{"robot", "bonirob"}});
  EXPECT_TRUE(e.eval(Query("robot == \"bonirob\"", {})));
  EXPECT_FALSE(e.eval(Query("robot == \"other\"", {})));
}

TEST(Engine, EvalNumericComparison) {
  Engine e;
  // try_number turns "42" into a Lua number, so > comparison works.
  e.set(Metadata{{"count", "42"}});
  EXPECT_TRUE(e.eval(Query("count > 40", {})));
  EXPECT_FALSE(e.eval(Query("count > 50", {})));
  EXPECT_TRUE(e.eval(Query("count == 42", {})));
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
  EXPECT_FALSE(e.eval(Query("", {})));
}

TEST(Engine, EvalGarbageReturnsFalseNoThrow) {
  Engine e;
  e.set(Metadata{{"robot", "x"}});
  // Undefined global → comparing nil with string raises a Lua error →
  // caught → false. (Note: missing key compares nil.)
  EXPECT_FALSE(e.eval(Query("undefined_key == \"x\"", {})));
  // Pure syntax garbage.
  EXPECT_FALSE(e.eval(std::string_view{")))"}));
}

TEST(Engine, ClearRemovesGlobals) {
  Engine e;
  Metadata md{{"robot", "bonirob"}};
  e.set(md);
  EXPECT_TRUE(e.eval(Query("robot == \"bonirob\"", {})));
  e.clear(md);
  // After clear, robot is nil → comparison errors → false.
  EXPECT_FALSE(e.eval(Query("robot == \"bonirob\"", {})));
}

TEST(Engine, ValidateGoodSyntax) {
  auto r = Engine::validate("robot == \"bonirob\"");
  EXPECT_TRUE(r.valid);
  EXPECT_TRUE(r.error.empty());
}

TEST(Engine, ValidateEmptyIsInvalid) {
  auto r = Engine::validate("");
  EXPECT_FALSE(r.valid);
  EXPECT_EQ(r.error, "empty query");
}

TEST(Engine, ValidateSyntaxErrorReportsLine) {
  // ")))" expands to raw, wrapped "return ()))" → Lua syntax error.
  auto r = Engine::validate(")))");
  EXPECT_FALSE(r.valid);
  EXPECT_FALSE(r.error.empty());
  // parse_error extracts a line number from the "...:N:..." message.
  EXPECT_GE(r.line, 1);
}

TEST(Engine, ValidatePartialIsValidLuaButFalsy) {
  // "robot ==" is incomplete for the parser, falls back to raw source
  // "robot ==", wrapped "return (robot ==)" → Lua SYNTAX ERROR → invalid.
  auto r = Engine::validate("robot ==");
  EXPECT_FALSE(r.valid);
}

TEST(Engine, ThreadLocalReuseAcrossValidateCalls) {
  // validate() reuses a thread_local sol::state. Repeated calls must be
  // independent (a prior failed load must not corrupt the next).
  EXPECT_FALSE(Engine::validate(")))").valid);
  EXPECT_TRUE(Engine::validate("robot == \"x\"").valid);
  EXPECT_FALSE(Engine::validate("=== bad").valid);
  EXPECT_TRUE(Engine::validate("a == 1 and b == 2").valid);
}

// ---------------------------------------------------------------------------
// applyCompletion (edit.h) — dropdown insertion/replace into the query text.
// Mirrors PJ3 QueryBar::applyInsert/applyEdit spacing: a leading space when the
// preceding char isn't whitespace, plus a trailing space; Replace overwrites
// the active token's [start, end) span.
// ---------------------------------------------------------------------------

TEST(ApplyCompletion, InsertKeyIntoEmptyText) {
  auto schema = makeSchema();
  auto ctx = analyze("", 0, schema);
  auto r = applyCompletion("", ctx, Action::Insert, "robot");
  EXPECT_EQ(r.text, "robot ");
  EXPECT_EQ(r.cursor, 6);
}

TEST(ApplyCompletion, InsertOperatorAfterKeyAddsLeadingSpace) {
  auto schema = makeSchema();
  // Cursor at end of "robot" (on the key token); the Op dropdown Inserts.
  auto ctx = analyze("robot", 5, schema);
  auto r = applyCompletion("robot", ctx, Action::Insert, "==");
  EXPECT_EQ(r.text, "robot == ");
  EXPECT_EQ(r.cursor, 9);
}

TEST(ApplyCompletion, InsertValueAfterOperator) {
  auto schema = makeSchema();
  std::string text = "robot ==";
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  auto r = applyCompletion(text, ctx, Action::Insert, "bonirob");
  EXPECT_EQ(r.text, "robot == bonirob ");
  EXPECT_EQ(r.cursor, 17);
}

TEST(ApplyCompletion, ReplaceKeyTokenPreservesRest) {
  auto schema = makeSchema();
  std::string text = "robt == \"x\"";
  // Cursor inside the misspelled key "robt" [0,4) → key_action == Replace.
  auto ctx = analyze(text, 2, schema);
  ASSERT_EQ(ctx.key_action, Action::Replace);
  auto r = applyCompletion(text, ctx, Action::Replace, "robot");
  EXPECT_EQ(r.text, "robot == \"x\"");
  EXPECT_EQ(r.cursor, 5);  // end of the inserted key, before the existing space
}

TEST(ApplyCompletion, ReplaceOperatorToken) {
  auto schema = makeSchema();
  std::string text = "robot = \"x\"";
  // Cursor on the single "=" operator [6,7) → op_action == Replace.
  auto ctx = analyze(text, 7, schema);
  ASSERT_EQ(ctx.op_action, Action::Replace);
  auto r = applyCompletion(text, ctx, Action::Replace, "==");
  EXPECT_EQ(r.text, "robot == \"x\"");
}

TEST(ApplyCompletion, InsertDoesNotDoubleSpaceWhenPrecededByWhitespace) {
  auto schema = makeSchema();
  std::string text = "robot == ";  // already has a trailing space
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  auto r = applyCompletion(text, ctx, Action::Insert, "bonirob");
  EXPECT_EQ(r.text, "robot == bonirob ");  // single space, not double
}

TEST(ApplyCompletion, ReplaceTokenAtEndAddsTrailingSpace) {
  auto schema = makeSchema();
  std::string text = "robot";  // single key token at the end of the text
  auto ctx = analyze(text, 5, schema);
  ASSERT_EQ(ctx.key_action, Action::Replace);
  // PJ3 applyEdit appends a trailing space when the replaced span ends the text.
  auto r = applyCompletion(text, ctx, Action::Replace, "sensor");
  EXPECT_EQ(r.text, "sensor ");
}

TEST(ApplyCompletion, InsertAfterMidTokenCaretLandsPastTheToken) {
  auto schema = makeSchema();
  // Caret in the middle of the key ("wea|ther"); the Op dropdown Inserts. The
  // insert must land after the whole token, not splice it ("wea == ther").
  auto ctx = analyze("weather", 3, schema);
  ASSERT_EQ(ctx.op_action, Action::Insert);
  auto r = applyCompletion("weather", ctx, Action::Insert, "==");
  EXPECT_EQ(r.text, "weather == ");
  EXPECT_EQ(r.cursor, 11);
}

TEST(QuoteValueForQuery, QuotesStringsLeavesNumbersBare) {
  EXPECT_EQ(quoteValueForQuery("cloudy"), "\"cloudy\"");
  EXPECT_EQ(quoteValueForQuery("sunny day"), "\"sunny day\"");
  EXPECT_EQ(quoteValueForQuery("42"), "42");
  EXPECT_EQ(quoteValueForQuery("3.14"), "3.14");
  EXPECT_EQ(quoteValueForQuery("-7"), "-7");
}

TEST(QuoteValueForQuery, EscapesEmbeddedQuotesAndBackslashes) {
  EXPECT_EQ(quoteValueForQuery("a\"b"), "\"a\\\"b\"");
  EXPECT_EQ(quoteValueForQuery("a\\b"), "\"a\\\\b\"");
}

// End-to-end: replacing a string value re-quotes it (the reported bug —
// picking "cloudy" over "sunny" produced a bare, un-quoted key).
TEST(ApplyCompletion, ReplaceStringValueStaysQuoted) {
  auto schema = makeSchema();
  std::string text = "robot == \"sunny\"";
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  ASSERT_EQ(ctx.val_action, Action::Replace);
  auto r = applyCompletion(text, ctx, ctx.val_action, quoteValueForQuery("cloudy"));
  EXPECT_EQ(r.text, "robot == \"cloudy\" ");
}

}  // namespace
