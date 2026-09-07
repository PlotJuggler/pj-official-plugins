// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pj_base/number_parse.hpp"
#include "pj_query/complete.hpp"
#include "pj_query/token.hpp"
#include "pj_query/types.hpp"

namespace PJ::query {

// What a dropdown action would do at this cursor position.
enum class Action {
  kDisabled,  // dropdown should be greyed out
  kInsert,    // append new text at cursor (no existing token to replace)
  kReplace,   // overwrite the token the cursor is on
};

// Full analysis of what the cursor position means for the UI.
// Computed once per cursor move, used by all three dropdowns.
struct CursorContext {
  int cursor = 0;

  // The token the cursor is on or touching.
  // -1 if cursor is in whitespace between tokens.
  int token_index = -1;
  Token active_token;  // valid only when token_index >= 0

  // From complete() on the prefix up to cursor.
  Expect expect = Expect::kAny;
  std::string context_key;
  int suggestion_count = 0;

  // What each dropdown would do.
  Action key_action = Action::kDisabled;
  Action op_action = Action::kDisabled;
  Action val_action = Action::kDisabled;

  // Convenience: is the dropdown usable at all?
  [[nodiscard]] bool canPickKey() const {
    return key_action != Action::kDisabled;
  }
  [[nodiscard]] bool canPickOp() const {
    return op_action != Action::kDisabled;
  }
  [[nodiscard]] bool canPickValue() const {
    return val_action != Action::kDisabled;
  }
};

// Find the token whose span touches the cursor.
// Cursor at position P is "on" a token if P is in [start, end].
// This means the cursor right after a token (P == end) is still on it —
// the common case when the user just finished typing a word.
// Returns token index, or -1 if cursor is in whitespace.
[[nodiscard]] inline int findActiveToken(const std::vector<Token>& tokens, int cursor) {
  for (int index = 0; index < static_cast<int>(tokens.size()); ++index) {
    if (cursor >= tokens[static_cast<std::size_t>(index)].start &&
        cursor <= tokens[static_cast<std::size_t>(index)].end) {
      return index;
    }
  }
  return -1;
}

// Compute the full cursor context for a position in the query text.
[[nodiscard]] inline CursorContext analyze(std::string_view text, int cursor, const Schema& schema) {
  CursorContext ctx;
  ctx.cursor = cursor;

  // Tokenize the FULL text once; both the active-token lookup and complete()
  // consume slices of this stream.
  Lexer lex(text);
  auto tokens = lex.tokenize();

  ctx.token_index = findActiveToken(tokens, cursor);
  if (ctx.token_index >= 0) {
    ctx.active_token = tokens[static_cast<std::size_t>(ctx.token_index)];
  }

  // Slice the stream at the cursor: everything up to and including the active
  // token (which the Lexer saw in full — "rob|ot" analyzes as "robot"), or
  // just the tokens fully before a cursor sitting in whitespace.
  std::vector<Token> before_cursor;
  if (ctx.token_index >= 0) {
    before_cursor.assign(tokens.begin(), tokens.begin() + ctx.token_index + 1);
  } else {
    for (const auto& token : tokens) {
      if (token.end < cursor) {
        before_cursor.push_back(token);
      }
    }
  }
  auto comp = complete(before_cursor, schema, /*last_token_complete=*/ctx.token_index >= 0);
  ctx.expect = comp.expect;
  ctx.context_key = comp.current_key;
  ctx.suggestion_count = static_cast<int>(comp.suggestions.size());

  // --- Derive actions ---
  // Replace: cursor is on an existing token of the matching type.
  // Insert:  expect says this token type goes next.
  // Both can be true (cursor on key → key=Replace AND op=Insert).

  bool on_key = ctx.token_index >= 0 && ctx.active_token.type == TokenType::kKey;
  bool on_op = ctx.token_index >= 0 && ctx.active_token.type == TokenType::kOperator;
  bool on_value = ctx.token_index >= 0 && ctx.active_token.type == TokenType::kValue;

  // Key dropdown:
  //   Replace: cursor is on a Key token
  //   Insert:  expect is Key, Any, or Connective (auto-chain)
  if (on_key) {
    ctx.key_action = Action::kReplace;
  } else if (ctx.expect == Expect::kKey || ctx.expect == Expect::kAny || ctx.expect == Expect::kConnective) {
    ctx.key_action = Action::kInsert;
  }

  // Op dropdown:
  //   Replace: cursor is on an Operator token
  //   Insert:  expect is Operator (i.e., cursor on a key → op next)
  if (on_op) {
    ctx.op_action = Action::kReplace;
  } else if (ctx.expect == Expect::kOperator) {
    ctx.op_action = Action::kInsert;
  }

  // Value dropdown:
  //   Replace: cursor is on a Value token
  //   Insert:  expect is Value (i.e., cursor on an operator → value next)
  if (on_value) {
    ctx.val_action = Action::kReplace;
  } else if (ctx.expect == Expect::kValue) {
    ctx.val_action = Action::kInsert;
  }

  return ctx;
}

// --- String formatting for debug display ---

[[nodiscard]] inline const char* actionStr(Action first) {
  switch (first) {
    case Action::kDisabled:
      return "\xe2\x80\x94";  // em-dash
    case Action::kInsert:
      return "INSERT";
    case Action::kReplace:
      return "REPLACE";
  }
  return "?";
}

[[nodiscard]] inline const char* expectStr(Expect entry) {
  switch (entry) {
    case Expect::kKey:
      return "Key";
    case Expect::kOperator:
      return "Operator";
    case Expect::kValue:
      return "Value";
    case Expect::kConnective:
      return "Connective";
    case Expect::kAny:
      return "Any";
  }
  return "?";
}

[[nodiscard]] inline const char* tokenTypeStr(TokenType token) {
  switch (token) {
    case TokenType::kKey:
      return "Key";
    case TokenType::kOperator:
      return "Op";
    case TokenType::kValue:
      return "Val";
    case TokenType::kAnd:
      return "And";
    case TokenType::kOr:
      return "Or";
    case TokenType::kNot:
      return "Not";
    case TokenType::kOpenParen:
      return "(";
    case TokenType::kCloseParen:
      return ")";
  }
  return "?";
}

// Format the cursor context as a two-line debug string.
[[nodiscard]] inline std::string formatDebug(const CursorContext& ctx) {
  std::string line1 = "pos:" + std::to_string(ctx.cursor);

  if (ctx.token_index >= 0) {
    line1 += "  active:\"" + ctx.active_token.text + "\" " + tokenTypeStr(ctx.active_token.type) + "[" +
             std::to_string(ctx.active_token.start) + "," + std::to_string(ctx.active_token.end) + ")";
  } else {
    line1 += "  active:(none)";
  }

  line1 += "  expect:" + std::string(expectStr(ctx.expect));

  if (!ctx.context_key.empty()) {
    line1 += "  ctx_key:" + ctx.context_key;
  }

  line1 += "  sugg:" + std::to_string(ctx.suggestion_count);

  std::string line2 = "key:" + std::string(actionStr(ctx.key_action));
  if (ctx.key_action == Action::kReplace) {
    line2 += "[" + std::to_string(ctx.active_token.start) + "," + std::to_string(ctx.active_token.end) + ")";
  }

  line2 += "  op:" + std::string(actionStr(ctx.op_action));
  if (ctx.op_action == Action::kReplace) {
    line2 += "[" + std::to_string(ctx.active_token.start) + "," + std::to_string(ctx.active_token.end) + ")";
  }

  line2 += "  val:" + std::string(actionStr(ctx.val_action));
  if (ctx.val_action == Action::kReplace) {
    line2 += "[" + std::to_string(ctx.active_token.start) + "," + std::to_string(ctx.active_token.end) + ")";
  }

  return line1 + "\n" + line2;
}

// --- Applying a dropdown completion to the query text ---

// Result of applying a completion: the new query text and the cursor offset
// just past the inserted token (after its trailing space).
struct EditResult {
  std::string text;
  int cursor = 0;
};

[[nodiscard]] inline bool isQueryWhitespace(char character) {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

// Format a schema value for insertion into the query. The Lexer classifies a
// bare word as a Key and a quoted word as a Value (token.hpp), so a string value
// like `cloudy` MUST be quoted to land as `weather == "cloudy"` — otherwise it
// lexes as a second key and the whole clause falls apart. Numeric values lex as
// Value bare, so they're left unquoted; embedded quotes/backslashes are escaped.
[[nodiscard]] inline std::string quoteValueForQuery(std::string_view value) {
  if (PJ::parseNumber<double>(value)) {
    return std::string(value);  // parses fully as a number → insert bare
  }
  std::string out;
  out.reserve(value.size() + 2);
  out += '"';
  for (char character : value) {
    if (character == '"' || character == '\\') {
      out += '\\';
    }
    out += character;
  }
  out += '"';
  return out;
}

// Splice the chosen dropdown `item` into `text` using the cursor context.
// `action` is the action of the dropdown that fired (ctx.key_action /
// op_action / val_action):
//   - Insert  → `item` is inserted at the cursor.
//   - Replace → the active token's [start, end) span is overwritten.
// Spacing mirrors PJ3 QueryBar::applyInsert / applyEdit: a leading space is
// added when the preceding char isn't whitespace; a trailing space is always
// added on Insert, and on Replace unless the span is already followed by
// whitespace. The returned cursor lands just past the inserted text.
[[nodiscard]] inline EditResult applyCompletion(
    std::string_view text, const CursorContext& ctx, Action action, std::string_view item) {
  std::string out(text);
  const int length = static_cast<int>(out.size());

  const bool replace = (action == Action::kReplace && ctx.token_index >= 0);
  // Insert: when the caret sits on a token, splice in *after* that token rather
  // than at the raw caret — a mid-token caret would otherwise cut the word in
  // half (e.g. picking "==" at "wea|ther" → "wea == ther"). In whitespace
  // (no active token), insert at the caret. Replace: overwrite the token span.
  int from;
  int to;
  if (replace) {
    from = ctx.active_token.start;
    to = ctx.active_token.end;
  } else if (ctx.token_index >= 0) {
    from = to = ctx.active_token.end;
  } else {
    from = to = ctx.cursor;
  }
  // Clamp defensively against a stale cursor / token span.
  from = from < 0 ? 0 : (from > length ? length : from);
  to = to < from ? from : (to > length ? length : to);

  std::string piece;
  if (from > 0 && !isQueryWhitespace(out[static_cast<std::size_t>(from) - 1])) {
    piece += ' ';
  }
  piece += std::string(item);
  const bool at_end = to >= length;
  if (!replace || at_end || !isQueryWhitespace(out[static_cast<std::size_t>(to)])) {
    piece += ' ';
  }

  out.replace(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from), piece);
  return EditResult{std::move(out), from + static_cast<int>(piece.size())};
}

}  // namespace PJ::query
