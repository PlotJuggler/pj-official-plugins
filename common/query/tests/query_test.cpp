// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "pj_query/query.hpp"

#include "gtest/gtest.h"
#include "pj_query/ast.hpp"
#include "pj_query/complete.hpp"
#include "pj_query/edit.hpp"
#include "pj_query/token.hpp"
#include "pj_query/types.hpp"

namespace PJ::query {
namespace {

Schema makeSchema() {
  return Schema{
      {"robot", {"bonirob", "other"}},
      {"sensor", {"camera", "laser"}},
  };
}

TEST(Lexer, TwoCharOperators) {
  auto toks = Lexer("a == b ~= c <= d >= e").tokenize();
  // a Op b Op c Op d Op e
  ASSERT_EQ(toks.size(), 9u);
  EXPECT_EQ(toks[1].type, TokenType::kOperator);
  EXPECT_EQ(toks[1].text, "==");
  EXPECT_EQ(toks[3].text, "~=");
  EXPECT_EQ(toks[5].text, "<=");
  EXPECT_EQ(toks[7].text, ">=");
}

TEST(Lexer, SingleCharComparisonOperators) {
  auto toks = Lexer("a < b > c").tokenize();
  ASSERT_EQ(toks.size(), 5u);
  EXPECT_EQ(toks[1].type, TokenType::kOperator);
  EXPECT_EQ(toks[1].text, "<");
  EXPECT_EQ(toks[3].type, TokenType::kOperator);
  EXPECT_EQ(toks[3].text, ">");
}

TEST(Lexer, LoneEqualsAndTildeAreOperators) {
  auto toks = Lexer("a = b").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[1].type, TokenType::kOperator);
  EXPECT_EQ(toks[1].text, "=");

  auto toks2 = Lexer("a ~ b").tokenize();
  ASSERT_EQ(toks2.size(), 3u);
  EXPECT_EQ(toks2[1].type, TokenType::kOperator);
  EXPECT_EQ(toks2[1].text, "~");
}

TEST(Lexer, QuotedStringsWithEscapes) {
  // Double and single quotes are Values; escaped quote does not terminate.
  auto toks = Lexer("robot == \"boni\\\"rob\"").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::kValue);
  EXPECT_EQ(toks[2].text, "\"boni\\\"rob\"");

  auto toks2 = Lexer("k == 'val'").tokenize();
  ASSERT_EQ(toks2.size(), 3u);
  EXPECT_EQ(toks2[2].type, TokenType::kValue);
  EXPECT_EQ(toks2[2].text, "'val'");
}

TEST(Lexer, UnterminatedQuoteConsumesToEnd) {
  auto toks = Lexer("k == \"unterminated").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::kValue);
  EXPECT_EQ(toks[2].text, "\"unterminated");
}

TEST(Lexer, NumericLiteralsClassifyAsValue) {
  // The whole word must parse as a number.
  auto toks = Lexer("count >= 42").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[2].type, TokenType::kValue);
  EXPECT_EQ(toks[2].text, "42");

  auto toks2 = Lexer("x == 3.14").tokenize();
  EXPECT_EQ(toks2[2].type, TokenType::kValue);

  auto toks3 = Lexer("x == -1.5e3").tokenize();
  EXPECT_EQ(toks3[2].type, TokenType::kValue);
}

TEST(Lexer, NonNumericWordsClassifyAsKey) {
  auto toks = Lexer("robot_type").tokenize();
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].type, TokenType::kKey);
  EXPECT_EQ(toks[0].text, "robot_type");

  // Starts numeric but isn't fully numeric.
  auto toks2 = Lexer("3abc").tokenize();
  ASSERT_EQ(toks2.size(), 1u);
  EXPECT_EQ(toks2[0].type, TokenType::kKey);
}

TEST(Lexer, KeywordsAndOrNot) {
  auto toks = Lexer("a and b or not c").tokenize();
  ASSERT_EQ(toks.size(), 6u);
  EXPECT_EQ(toks[1].type, TokenType::kAnd);
  EXPECT_EQ(toks[3].type, TokenType::kOr);
  EXPECT_EQ(toks[4].type, TokenType::kNot);
}

TEST(Lexer, Parens) {
  auto toks = Lexer("( a )").tokenize();
  ASSERT_EQ(toks.size(), 3u);
  EXPECT_EQ(toks[0].type, TokenType::kOpenParen);
  EXPECT_EQ(toks[0].text, "(");
  EXPECT_EQ(toks[2].type, TokenType::kCloseParen);
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
  // \x01 is neither space nor punctuation, so it joins the "a" word: ["a\x01", "b"].
  auto toks = Lexer("a\x01 b").tokenize();
  ASSERT_EQ(toks.size(), 2u);
  EXPECT_EQ(toks[1].text, "b");
}

TEST(Parser, FullClause) {
  auto toks = Lexer("robot == \"bonirob\"").tokenize();
  auto res = Parser(toks).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kCompare);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(res.error.empty());
  EXPECT_EQ(serialize(res.ast.get()), "robot == \"bonirob\"");
}

TEST(Parser, BareKeyIsKeyExprAndIncomplete) {
  auto res = Parser(Lexer("robot").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kKey);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot");
}

TEST(Parser, PartialKeyOpIsPartialExprAndIncomplete) {
  auto res = Parser(Lexer("robot ==").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kPartial);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "robot ==");
}

TEST(Parser, AndOrPrecedence) {
  // A or B and C parses as A or (B and C).
  auto res = Parser(Lexer("a == \"1\" or b == \"2\" and c == \"3\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kBinary);
  auto* top = static_cast<const BinaryExpr*>(res.ast.get());
  EXPECT_EQ(top->connective.type, TokenType::kOr);
  ASSERT_EQ(top->right->type, NodeType::kBinary);
  EXPECT_EQ(static_cast<const BinaryExpr*>(top->right.get())->connective.type, TokenType::kAnd);
  EXPECT_TRUE(res.complete);
}

TEST(Parser, NotExpr) {
  auto res = Parser(Lexer("not robot == \"x\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kNot);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "not robot == \"x\"");
}

TEST(Parser, NestedGroup) {
  auto res = Parser(Lexer("(a == \"1\" or b == \"2\")").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kGroup);
  EXPECT_TRUE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "(a == \"1\" or b == \"2\")");
}

TEST(Parser, EmptyParensYieldsNullAst) {
  // The inner parse skips the ")" as an unexpected token, so the group's own close-paren check
  // fails and the null inner is returned: a null AST, incomplete, no error.
  auto res = Parser(Lexer("()").tokenize()).parse();
  EXPECT_EQ(res.ast, nullptr);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(serialize(res.ast.get()), "");
}

TEST(Parser, UnclosedParenReturnsInner) {
  auto res = Parser(Lexer("(a == \"1\"").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kCompare);
  EXPECT_TRUE(res.complete);  // the inner compare is complete, no leftover tokens
}

TEST(Parser, ShorthandExpansion) {
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
  // The parser stops at a non-connective, so "foo" is leftover.
  auto res = Parser(Lexer("a == \"1\" foo").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_FALSE(res.complete);
  EXPECT_EQ(res.error, "unexpected tokens");
}

TEST(Parser, DanglingConnectiveStopsCleanly) {
  // The "or" is consumed before the missing right side is detected, so no token is left over:
  // the trailing connective is dropped, not flagged.
  auto res = Parser(Lexer("a == \"1\" or").tokenize()).parse();
  ASSERT_TRUE(res.ast);
  EXPECT_EQ(res.ast->type, NodeType::kCompare);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(res.error.empty());
}

TEST(Query, LuaExpandsCompleteQuery) {
  Query query("robot == \"a\" or \"b\"");
  EXPECT_TRUE(query.complete());
  EXPECT_EQ(query.expanded(), "robot == \"a\" or robot == \"b\"");
  EXPECT_EQ(Query("robot == \"a\" and \"b\"").expanded(), "robot == \"a\" and robot == \"b\"");
  // No shorthand: serialized verbatim.
  EXPECT_EQ(Query("robot == \"a\"").expanded(), "robot == \"a\"");
}

TEST(Query, LuaFallsBackToRawForIncomplete) {
  Query query("robot ==");
  EXPECT_FALSE(query.complete());
  EXPECT_EQ(query.expanded(), "robot ==");

  Query bare("robot");
  EXPECT_FALSE(bare.complete());
  EXPECT_EQ(bare.expanded(), "robot");
}

TEST(Query, EmptyQuery) {
  Query query("");
  EXPECT_TRUE(query.empty());
  EXPECT_TRUE(query.expanded().empty());
  EXPECT_EQ(query.ast(), nullptr);
}

TEST(Query, TokenAtAndIndexAt) {
  Query query("robot == \"x\"");
  // robot[0,5) ==[6,8) "x"[9,12)
  ASSERT_NE(query.tokenAt(0), nullptr);
  EXPECT_EQ(query.tokenAt(0)->text, "robot");
  EXPECT_EQ(query.tokenIndexAt(0), 0);
  // pos 5 is the space: token ends are exclusive.
  EXPECT_EQ(query.tokenAt(5), nullptr);
  EXPECT_EQ(query.tokenIndexAt(5), -1);
  EXPECT_EQ(query.tokenAt(6)->text, "==");
  EXPECT_EQ(query.tokenIndexAt(6), 1);
  EXPECT_EQ(query.tokenAt(10)->text, "\"x\"");
  EXPECT_EQ(query.tokenIndexAt(10), 2);
}

TEST(Query, KeyBefore) {
  Query query("robot == \"x\" and sensor ==");
  // Tokens: robot Op Val And sensor Op
  EXPECT_EQ(query.keyBefore(5), "sensor");
  EXPECT_EQ(query.keyBefore(1), "robot");
  EXPECT_EQ(query.keyBefore(0), "");
}

TEST(Query, ExpectedAtTransitions) {
  Query query("robot == \"x\"");
  EXPECT_EQ(query.expectedAt(-1), TokenType::kKey);
  // Inside "robot": editing a Key.
  EXPECT_EQ(query.expectedAt(2), TokenType::kKey);
  EXPECT_EQ(query.expectedAt(5), TokenType::kOperator);
  EXPECT_EQ(query.expectedAt(8), TokenType::kValue);
  EXPECT_EQ(query.expectedAt(12), TokenType::kAnd);
}

TEST(Query, ExpectedAtAfterConnectiveAndParen) {
  Query query("a == \"1\" and ( b == \"2\" )");
  const Token* and_tok = nullptr;
  for (const auto& token : query.tokens()) {
    if (token.type == TokenType::kAnd) {
      and_tok = &token;
    }
  }
  ASSERT_NE(and_tok, nullptr);
  EXPECT_EQ(query.expectedAt(and_tok->end), TokenType::kKey);
}

TEST(Complete, AtStartExpectsKeysAny) {
  auto schema = makeSchema();
  auto character = complete("", 0, schema);
  EXPECT_EQ(character.expect, Expect::kAny);
  // Suggestions are all schema keys.
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterKnownKeyExpectsOperator) {
  auto schema = makeSchema();
  auto character = complete("robot", 5, schema);
  EXPECT_EQ(character.expect, Expect::kOperator);
  EXPECT_EQ(character.current_key, "robot");
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"==", "~=", "<", ">", "<=", ">="}));
}

TEST(Complete, PartialKeyFiltersByPrefix) {
  auto schema = makeSchema();
  auto character = complete("rob", 3, schema);
  EXPECT_EQ(character.expect, Expect::kKey);
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"robot"}));
}

TEST(Complete, AfterOperatorExpectsThatKeysValues) {
  auto schema = makeSchema();
  auto character = complete("robot ==", 8, schema);
  EXPECT_EQ(character.expect, Expect::kValue);
  EXPECT_EQ(character.current_key, "robot");
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"bonirob", "other"}));

  auto c2 = complete("sensor ==", 9, schema);
  EXPECT_EQ(c2.current_key, "sensor");
  EXPECT_EQ(c2.suggestions, (std::vector<std::string>{"camera", "laser"}));
}

TEST(Complete, AfterValueExpectsConnective) {
  auto schema = makeSchema();
  // key op value → after the value, expect a connective (and/or).
  auto character = complete("robot == \"bonirob\"", 18, schema);
  EXPECT_EQ(character.expect, Expect::kConnective);
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"and", "or"}));
}

TEST(Complete, AfterConnectiveExpectsKey) {
  auto schema = makeSchema();
  auto character = complete("robot == \"bonirob\" and", 22, schema);
  EXPECT_EQ(character.expect, Expect::kKey);
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterOpenParenExpectsKey) {
  auto schema = makeSchema();
  auto character = complete("(", 1, schema);
  EXPECT_EQ(character.expect, Expect::kKey);
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"robot", "sensor"}));
}

TEST(Complete, AfterCloseParenExpectsConnective) {
  auto schema = makeSchema();
  auto character = complete("(robot == \"x\")", 14, schema);
  EXPECT_EQ(character.expect, Expect::kConnective);
  EXPECT_EQ(character.suggestions, (std::vector<std::string>{"and", "or"}));
}

TEST(FindActiveToken, BoundaryAtTokenEnd) {
  // The cursor is "on" a token when it is in [start, end]; the end is inclusive.
  auto toks = Lexer("robot == \"x\"").tokenize();
  // robot is [0,5).
  EXPECT_EQ(findActiveToken(toks, 5), 0);
  EXPECT_EQ(findActiveToken(toks, 0), 0);
  EXPECT_EQ(findActiveToken(toks, 6), 1);
}

TEST(Analyze, AtStartExpectsAnyKeyInsert) {
  auto schema = makeSchema();
  auto ctx = analyze("", 0, schema);
  EXPECT_EQ(ctx.token_index, -1);
  EXPECT_EQ(ctx.expect, Expect::kAny);
  // Key dropdown inserts at Any; op/val disabled.
  EXPECT_EQ(ctx.key_action, Action::kInsert);
  EXPECT_EQ(ctx.op_action, Action::kDisabled);
  EXPECT_EQ(ctx.val_action, Action::kDisabled);
  EXPECT_TRUE(ctx.canPickKey());
  EXPECT_FALSE(ctx.canPickOp());
  EXPECT_FALSE(ctx.canPickValue());
}

TEST(Analyze, CursorOnKeyReplacesKeyInsertsOp) {
  auto schema = makeSchema();
  // Cursor at end of "robot" (pos 5) — on the Key token.
  auto ctx = analyze("robot", 5, schema);
  ASSERT_EQ(ctx.token_index, 0);
  EXPECT_EQ(ctx.active_token.type, TokenType::kKey);
  // On a Key: expect = Operator, context_key = the key.
  EXPECT_EQ(ctx.expect, Expect::kOperator);
  EXPECT_EQ(ctx.context_key, "robot");
  // Key dropdown Replaces (cursor on key); Op dropdown Inserts (op next).
  EXPECT_EQ(ctx.key_action, Action::kReplace);
  EXPECT_EQ(ctx.op_action, Action::kInsert);
  EXPECT_EQ(ctx.val_action, Action::kDisabled);
}

TEST(Analyze, CursorOnOperatorReplacesOpInsertsValue) {
  auto schema = makeSchema();
  // "robot ==" — cursor at end (pos 8) on the operator.
  auto ctx = analyze("robot ==", 8, schema);
  ASSERT_EQ(ctx.token_index, 1);
  EXPECT_EQ(ctx.active_token.type, TokenType::kOperator);
  EXPECT_EQ(ctx.expect, Expect::kValue);
  EXPECT_EQ(ctx.context_key, "robot");  // key before the operator
  EXPECT_EQ(ctx.op_action, Action::kReplace);
  EXPECT_EQ(ctx.val_action, Action::kInsert);
}

TEST(Analyze, CursorOnValueReplacesValueExpectConnective) {
  auto schema = makeSchema();
  // "robot == \"x\"" cursor at end (pos 12) on the Value token.
  auto ctx = analyze("robot == \"x\"", 12, schema);
  ASSERT_EQ(ctx.token_index, 2);
  EXPECT_EQ(ctx.active_token.type, TokenType::kValue);
  EXPECT_EQ(ctx.expect, Expect::kConnective);
  EXPECT_EQ(ctx.context_key, "robot");
  EXPECT_EQ(ctx.val_action, Action::kReplace);
  // Connective → key dropdown can auto-chain (Insert).
  EXPECT_EQ(ctx.key_action, Action::kInsert);
}

TEST(Analyze, ReplaceRangeMatchesActiveToken) {
  auto schema = makeSchema();
  auto ctx = analyze("robot", 3, schema);  // cursor inside "robot"
  ASSERT_EQ(ctx.token_index, 0);
  // The replacement range is the active token's [start, end).
  EXPECT_EQ(ctx.active_token.start, 0);
  EXPECT_EQ(ctx.active_token.end, 5);
  EXPECT_EQ(ctx.key_action, Action::kReplace);
}

TEST(Analyze, WhitespaceBetweenTokensNoActiveToken) {
  auto schema = makeSchema();
  // Position 5 is still "on" robot (inclusive end); 6 is the gap between the two spaces.
  auto ctx = analyze("robot  ==", 6, schema);
  EXPECT_EQ(ctx.token_index, -1);
}

TEST(ApplyCompletion, InsertKeyIntoEmptyText) {
  auto schema = makeSchema();
  auto ctx = analyze("", 0, schema);
  auto right_expr = applyCompletion("", ctx, Action::kInsert, "robot");
  EXPECT_EQ(right_expr.text, "robot ");
  EXPECT_EQ(right_expr.cursor, 6);
}

TEST(ApplyCompletion, InsertOperatorAfterKeyAddsLeadingSpace) {
  auto schema = makeSchema();
  // Cursor at end of "robot" (on the key token); the Op dropdown Inserts.
  auto ctx = analyze("robot", 5, schema);
  auto right_expr = applyCompletion("robot", ctx, Action::kInsert, "==");
  EXPECT_EQ(right_expr.text, "robot == ");
  EXPECT_EQ(right_expr.cursor, 9);
}

TEST(ApplyCompletion, InsertValueAfterOperator) {
  auto schema = makeSchema();
  std::string text = "robot ==";
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  auto right_expr = applyCompletion(text, ctx, Action::kInsert, "bonirob");
  EXPECT_EQ(right_expr.text, "robot == bonirob ");
  EXPECT_EQ(right_expr.cursor, 17);
}

TEST(ApplyCompletion, ReplaceKeyTokenPreservesRest) {
  auto schema = makeSchema();
  std::string text = "robt == \"x\"";
  // Cursor inside the misspelled key "robt" [0,4) → key_action == Replace.
  auto ctx = analyze(text, 2, schema);
  ASSERT_EQ(ctx.key_action, Action::kReplace);
  auto right_expr = applyCompletion(text, ctx, Action::kReplace, "robot");
  EXPECT_EQ(right_expr.text, "robot == \"x\"");
  EXPECT_EQ(right_expr.cursor, 5);  // end of the inserted key, before the existing space
}

TEST(ApplyCompletion, ReplaceOperatorToken) {
  auto schema = makeSchema();
  std::string text = "robot = \"x\"";
  // Cursor on the single "=" operator [6,7) → op_action == Replace.
  auto ctx = analyze(text, 7, schema);
  ASSERT_EQ(ctx.op_action, Action::kReplace);
  auto right_expr = applyCompletion(text, ctx, Action::kReplace, "==");
  EXPECT_EQ(right_expr.text, "robot == \"x\"");
}

TEST(ApplyCompletion, InsertDoesNotDoubleSpaceWhenPrecededByWhitespace) {
  auto schema = makeSchema();
  std::string text = "robot == ";  // already has a trailing space
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  auto right_expr = applyCompletion(text, ctx, Action::kInsert, "bonirob");
  EXPECT_EQ(right_expr.text, "robot == bonirob ");  // single space, not double
}

TEST(ApplyCompletion, ReplaceTokenAtEndAddsTrailingSpace) {
  auto schema = makeSchema();
  std::string text = "robot";  // single key token at the end of the text
  auto ctx = analyze(text, 5, schema);
  ASSERT_EQ(ctx.key_action, Action::kReplace);
  // PJ3 applyEdit appends a trailing space when the replaced span ends the text.
  auto right_expr = applyCompletion(text, ctx, Action::kReplace, "sensor");
  EXPECT_EQ(right_expr.text, "sensor ");
}

TEST(ApplyCompletion, InsertAfterMidTokenCaretLandsPastTheToken) {
  auto schema = makeSchema();
  // The insert must land after the whole token, not splice it ("wea == ther").
  auto ctx = analyze("weather", 3, schema);
  ASSERT_EQ(ctx.op_action, Action::kInsert);
  auto right_expr = applyCompletion("weather", ctx, Action::kInsert, "==");
  EXPECT_EQ(right_expr.text, "weather == ");
  EXPECT_EQ(right_expr.cursor, 11);
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

// A replaced string value must be re-quoted: a bare word lexes as a Key.
TEST(ApplyCompletion, ReplaceStringValueStaysQuoted) {
  auto schema = makeSchema();
  std::string text = "robot == \"sunny\"";
  auto ctx = analyze(text, static_cast<int>(text.size()), schema);
  ASSERT_EQ(ctx.val_action, Action::kReplace);
  auto right_expr = applyCompletion(text, ctx, ctx.val_action, quoteValueForQuery("cloudy"));
  EXPECT_EQ(right_expr.text, "robot == \"cloudy\" ");
}

}  // namespace

}  // namespace PJ::query

TEST(QueryEditingHelpers, DebugNamesAndStaleCursorBounds) {
  using namespace PJ::query;
  EXPECT_STREQ(actionStr(Action::kDisabled), "\xe2\x80\x94");
  EXPECT_STREQ(actionStr(Action::kInsert), "INSERT");
  EXPECT_STREQ(actionStr(Action::kReplace), "REPLACE");
  EXPECT_STREQ(expectStr(Expect::kKey), "Key");
  EXPECT_STREQ(expectStr(Expect::kOperator), "Operator");
  EXPECT_STREQ(expectStr(Expect::kValue), "Value");
  EXPECT_STREQ(expectStr(Expect::kConnective), "Connective");
  EXPECT_STREQ(expectStr(Expect::kAny), "Any");
  EXPECT_STREQ(tokenTypeStr(TokenType::kKey), "Key");
  EXPECT_STREQ(tokenTypeStr(TokenType::kOperator), "Op");
  EXPECT_STREQ(tokenTypeStr(TokenType::kValue), "Val");
  EXPECT_STREQ(tokenTypeStr(TokenType::kAnd), "And");
  EXPECT_STREQ(tokenTypeStr(TokenType::kOr), "Or");
  EXPECT_STREQ(tokenTypeStr(TokenType::kNot), "Not");
  EXPECT_STREQ(tokenTypeStr(TokenType::kOpenParen), "(");
  EXPECT_STREQ(tokenTypeStr(TokenType::kCloseParen), ")");
  const auto context = analyze("robot", 2, {{"robot", {"a"}}});
  EXPECT_NE(formatDebug(context).find("pos:2"), std::string::npos);
  CursorContext stale;
  stale.cursor = 999;
  EXPECT_EQ(applyCompletion("robot", stale, Action::kInsert, "==").text, "robot == ");
  stale.cursor = -1;
  EXPECT_EQ(applyCompletion("", stale, Action::kInsert, "robot").text, "robot ");
}
