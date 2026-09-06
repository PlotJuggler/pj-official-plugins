// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pj_query/ast.hpp"
#include "pj_query/token.hpp"

namespace PJ::query {

/// Immutable language/editing analysis. complete() describes full comparisons
/// in the editing grammar; use the plugin Evaluator for definitive validation.
/// expanded() serializes shorthand or preserves raw incomplete input.
class Query {
 public:
  Query() = default;

  explicit Query(std::string_view source) : source_(source) {
    Lexer lexer(source);
    tokens_ = lexer.tokenize();

    Parser parser(tokens_);
    auto result = parser.parse();
    ast_ = std::move(result.ast);
    complete_ = result.complete;
    parse_error_ = std::move(result.error);

    // If the parse is complete (all leaves are full comparisons), use the
    // expanded serialization. Otherwise fall back to the raw source — the
    // query may be valid Lua that our subset parser doesn't understand
    // (e.g., string.find(), nil comparisons, function calls).
    if (complete_) {
      expanded_ = serialize(ast_.get());
    } else {
      expanded_ = source_;
    }
  }

  // The original source string.
  [[nodiscard]] const std::string& source() const {
    return source_;
  }

  // The token stream from lexing.
  [[nodiscard]] const std::vector<Token>& tokens() const {
    return tokens_;
  }

  // The parsed AST. Null if the input was empty.
  [[nodiscard]] const Expr* ast() const {
    return ast_.get();
  }

  // True if every leaf is a full CompareExpr (no bare keys or partials).
  [[nodiscard]] bool complete() const {
    return complete_;
  }

  // Non-empty if parsing failed structurally.
  [[nodiscard]] const std::string& error() const {
    return parse_error_;
  }

  // Is there anything to evaluate?
  [[nodiscard]] bool empty() const {
    return tokens_.empty();
  }

  // Expanded expression text; incomplete input is preserved for the local evaluator.
  [[nodiscard]] const std::string& expanded() const {
    return expanded_;
  }

  // Find the token at a given character offset in the source.
  // Returns nullptr if no token covers that position.
  [[nodiscard]] const Token* tokenAt(int pos) const {
    for (const auto& tok : tokens_) {
      if (pos >= tok.start && pos < tok.end) {
        return &tok;
      }
    }
    return nullptr;
  }

  // Find the token index at a given character offset.
  // Returns -1 if no token covers that position.
  [[nodiscard]] int tokenIndexAt(int pos) const {
    for (int index = 0; index < static_cast<int>(tokens_.size()); ++index) {
      if (pos >= tokens_[index].start && pos < tokens_[index].end) {
        return index;
      }
    }
    return -1;
  }

  // Get the most recent key in context before a given token index.
  // Walks backward through tokens to find the nearest Key token.
  [[nodiscard]] std::string keyBefore(int token_index) const {
    for (int index = token_index - 1; index >= 0; --index) {
      if (tokens_[index].type == TokenType::kKey) {
        return tokens_[index].text;
      }
    }
    return {};
  }

  // Determine what token type is expected at the given character offset.
  [[nodiscard]] TokenType expectedAt(int pos) const {
    // Find which token we're at or after.
    int idx = -1;
    for (int index = 0; index < static_cast<int>(tokens_.size()); ++index) {
      if (tokens_[index].start <= pos) {
        idx = index;
      } else {
        break;
      }
    }

    if (idx < 0) {
      return TokenType::kKey;  // empty or before first token
    }

    auto last_type = tokens_[idx].type;

    // If cursor is inside a token, we're editing that token type.
    if (pos < tokens_[idx].end) {
      return last_type;
    }

    // Cursor is after the token — what comes next?
    switch (last_type) {
      case TokenType::kKey:
        return TokenType::kOperator;
      case TokenType::kOperator:
        return TokenType::kValue;
      case TokenType::kValue:
        return TokenType::kAnd;  // connective
      case TokenType::kAnd:
      case TokenType::kOr:
      case TokenType::kNot:
      case TokenType::kOpenParen:
        return TokenType::kKey;
      case TokenType::kCloseParen:
        return TokenType::kAnd;  // connective
    }

    return TokenType::kKey;
  }

 private:
  std::string source_;
  std::vector<Token> tokens_;
  ExprPtr ast_;
  bool complete_ = false;
  std::string parse_error_;
  std::string expanded_;
};

}  // namespace PJ::query
