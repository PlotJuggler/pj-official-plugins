// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pj_query/token.hpp"

namespace PJ::query {

// --- AST node types ---

enum class NodeType {
  kCompare,  // key op value       (e.g., robot == "humanoid")
  kBinary,   // left and/or right  (e.g., A and B)
  kNot,      // not expr           (e.g., not (X))
  kGroup,    // ( expr )
  kKey,      // bare key, incomplete clause (e.g., robot)
  kPartial,  // key op, missing value (e.g., robot ==)
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Base expression node. All AST nodes carry their source range.
struct Expr {
  NodeType type;
  int start = 0;  // source offset of first token
  int end = 0;    // source offset past last token

  explicit Expr(NodeType token) : type(token) {}
  virtual ~Expr() = default;

  Expr(const Expr&) = delete;
  Expr& operator=(const Expr&) = delete;
  Expr(Expr&&) = default;
  Expr& operator=(Expr&&) = default;
};

// key op value
struct CompareExpr : Expr {
  Token key;
  Token op;
  Token value;

  CompareExpr(Token key_token, Token operator_token, Token value_token)
      : Expr(NodeType::kCompare),
        key(std::move(key_token)),
        op(std::move(operator_token)),
        value(std::move(value_token)) {
    start = key.start;
    end = value.end;
  }
};

// left (and|or) right
struct BinaryExpr : Expr {
  ExprPtr left;
  Token connective;  // the "and" / "or" token
  ExprPtr right;

  BinaryExpr(ExprPtr left_expr, Token conn, ExprPtr right_expr)
      : Expr(NodeType::kBinary), left(std::move(left_expr)), connective(std::move(conn)), right(std::move(right_expr)) {
    start = left->start;
    end = right->end;
  }
};

// not expr
struct NotExpr : Expr {
  Token not_token;
  ExprPtr operand;

  NotExpr(Token nt, ExprPtr op) : Expr(NodeType::kNot), not_token(std::move(nt)), operand(std::move(op)) {
    start = not_token.start;
    end = operand->end;
  }
};

// ( expr )
struct GroupExpr : Expr {
  Token open;
  ExprPtr inner;
  Token close;

  GroupExpr(Token open_token, ExprPtr inner_expr, Token close_token)
      : Expr(NodeType::kGroup),
        open(std::move(open_token)),
        inner(std::move(inner_expr)),
        close(std::move(close_token)) {
    start = open.start;
    end = close.end;
  }
};

// Bare key — incomplete clause (user is still typing).
struct KeyExpr : Expr {
  Token key;

  explicit KeyExpr(Token key_token) : Expr(NodeType::kKey), key(std::move(key_token)) {
    start = key.start;
    end = key.end;
  }
};

// key op — missing value (user is still typing).
struct PartialExpr : Expr {
  Token key;
  Token op;

  PartialExpr(Token key_token, Token operator_token)
      : Expr(NodeType::kPartial), key(std::move(key_token)), op(std::move(operator_token)) {
    start = key.start;
    end = op.end;
  }
};

// --- Parser ---
//
// Parses a token stream into an AST. Handles:
//   - Full clauses: key op value
//   - Partial clauses: bare key, key op (missing value)
//   - Connectives: and, or (left-to-right, or has lower precedence than and)
//   - Not: not expr
//   - Groups: ( expr )
//   - Shorthand: key op val1 or val2 → key op val1 or key op val2
//     (expanded during parsing by tracking last key+op)
//
// The parser never throws. Malformed input produces partial/key nodes.

struct ParseResult {
  ExprPtr ast;
  bool complete = false;  // true if every leaf is a full CompareExpr
  std::string error;      // non-empty if structurally broken
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  [[nodiscard]] ParseResult parse() {
    if (tokens_.empty()) {
      return {nullptr, false, "empty"};
    }

    pos_ = 0;
    auto expr = parseOr();

    // If there are leftover tokens, the parse is incomplete.
    bool leftover = pos_ < static_cast<int>(tokens_.size());

    bool all_complete = expr && checkComplete(expr.get());

    return {std::move(expr), all_complete && !leftover, leftover ? "unexpected tokens" : ""};
  }

 private:
  // or-level: lowest precedence
  ExprPtr parseOr() {
    auto left = parseAnd();
    if (!left) {
      return nullptr;
    }

    while (pos_ < size() && tok(pos_).type == TokenType::kOr) {
      auto conn = tok(pos_++);
      auto right = parseAnd();
      if (!right) {
        // "A or" with nothing after — still return what we have.
        break;
      }
      left = std::make_unique<BinaryExpr>(std::move(left), std::move(conn), std::move(right));
    }

    return left;
  }

  // and-level: higher precedence than or
  ExprPtr parseAnd() {
    auto left = parseUnary();
    if (!left) {
      return nullptr;
    }

    while (pos_ < size() && tok(pos_).type == TokenType::kAnd) {
      auto conn = tok(pos_++);
      auto right = parseUnary();
      if (!right) {
        break;
      }
      left = std::make_unique<BinaryExpr>(std::move(left), std::move(conn), std::move(right));
    }

    return left;
  }

  // not-level
  ExprPtr parseUnary() {
    if (pos_ < size() && tok(pos_).type == TokenType::kNot) {
      auto not_tok = tok(pos_++);
      auto operand = parseUnary();
      if (!operand) {
        return nullptr;
      }
      return std::make_unique<NotExpr>(std::move(not_tok), std::move(operand));
    }

    return parsePrimary();
  }

  // Primary: group, comparison, shorthand value, or bare key
  ExprPtr parsePrimary() {
    if (pos_ >= size()) {
      return nullptr;
    }

    // Grouped expression: ( expr )
    if (tok(pos_).type == TokenType::kOpenParen) {
      auto open = tok(pos_++);
      auto inner = parseOr();
      if (pos_ < size() && tok(pos_).type == TokenType::kCloseParen) {
        auto close = tok(pos_++);
        if (!inner) {
          // Empty parens — create a group with no inner.
          return std::make_unique<GroupExpr>(std::move(open), nullptr, std::move(close));
        }
        return std::make_unique<GroupExpr>(std::move(open), std::move(inner), std::move(close));
      }
      // Unclosed paren — return inner or null.
      return inner;
    }

    // Shorthand value: a bare value after connective inherits last_key + last_op.
    // e.g., robot == "a" or "b" → the "b" is parsed here as a shorthand compare.
    if (tok(pos_).type == TokenType::kValue && !last_key_.text.empty()) {
      auto val = tok(pos_++);
      return std::make_unique<CompareExpr>(last_key_, last_op_, std::move(val));
    }

    // Key — start of a comparison or bare key.
    if (tok(pos_).type == TokenType::kKey) {
      auto key = tok(pos_++);

      // Key + operator?
      if (pos_ < size() && tok(pos_).type == TokenType::kOperator) {
        auto op = tok(pos_++);

        // Key + operator + value?
        if (pos_ < size() && tok(pos_).type == TokenType::kValue) {
          auto val = tok(pos_++);

          // Remember for shorthand expansion.
          last_key_ = key;
          last_op_ = op;

          return std::make_unique<CompareExpr>(std::move(key), std::move(op), std::move(val));
        }

        // Key + operator, no value yet (partial).
        last_key_ = key;
        last_op_ = op;
        return std::make_unique<PartialExpr>(std::move(key), std::move(op));
      }

      // Bare key.
      return std::make_unique<KeyExpr>(std::move(key));
    }

    // Unexpected token — skip it to avoid infinite loop.
    ++pos_;
    return nullptr;
  }

  [[nodiscard]] static bool checkComplete(const Expr* expr) {
    if (!expr) {
      return false;
    }
    switch (expr->type) {
      case NodeType::kCompare:
        return true;
      case NodeType::kBinary: {
        auto* binary = static_cast<const BinaryExpr*>(expr);
        return checkComplete(binary->left.get()) && checkComplete(binary->right.get());
      }
      case NodeType::kNot:
        return checkComplete(static_cast<const NotExpr*>(expr)->operand.get());
      case NodeType::kGroup:
        return checkComplete(static_cast<const GroupExpr*>(expr)->inner.get());
      case NodeType::kKey:
      case NodeType::kPartial:
        return false;
    }
    return false;
  }

  // Centralizes the int → size_type cast so -Wsign-conversion stays clean
  // (positions are int end-to-end: they mirror Qt cursor offsets).
  [[nodiscard]] const Token& tok(int index) const {
    return tokens_[static_cast<std::size_t>(index)];
  }

  [[nodiscard]] int size() const {
    return static_cast<int>(tokens_.size());
  }

  std::vector<Token> tokens_;
  int pos_ = 0;

  // Last seen key+op for shorthand expansion.
  Token last_key_;
  Token last_op_;
};

// --- Helpers ---

// Serialize an AST back to a Lua-compatible string (with shorthand expanded).
inline std::string serialize(const Expr* expr) {
  if (!expr) {
    return {};
  }

  switch (expr->type) {
    case NodeType::kCompare: {
      auto* compare = static_cast<const CompareExpr*>(expr);
      return compare->key.text + " " + compare->op.text + " " + compare->value.text;
    }
    case NodeType::kBinary: {
      auto* binary = static_cast<const BinaryExpr*>(expr);
      return serialize(binary->left.get()) + " " + binary->connective.text + " " + serialize(binary->right.get());
    }
    case NodeType::kNot: {
      auto* negation = static_cast<const NotExpr*>(expr);
      return "not " + serialize(negation->operand.get());
    }
    case NodeType::kGroup: {
      auto* group_expr = static_cast<const GroupExpr*>(expr);
      return "(" + serialize(group_expr->inner.get()) + ")";
    }
    case NodeType::kKey:
      return static_cast<const KeyExpr*>(expr)->key.text;
    case NodeType::kPartial: {
      auto* partial_expr = static_cast<const PartialExpr*>(expr);
      return partial_expr->key.text + " " + partial_expr->op.text;
    }
  }
  return {};
}

}  // namespace PJ::query
