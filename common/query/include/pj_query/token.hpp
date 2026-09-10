// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/number_parse.hpp"

namespace PJ::query {

enum class TokenType {
  kKey,         // metadata key identifier (e.g., robot, sensor_type)
  kOperator,    // ==  ~=  <  >  <=  >=
  kValue,       // string literal "..." or '...' or numeric literal
  kAnd,         // and
  kOr,          // or
  kNot,         // not
  kOpenParen,   // (
  kCloseParen,  // )
};

struct Token {
  TokenType type;
  std::string text;  // raw source text of this token
  int start = 0;     // byte offset in source (inclusive)
  int end = 0;       // byte offset in source (exclusive)
};

// Lexer: turns a query string into a sequence of typed, positioned tokens.
//
// Classification rules:
//   - "and", "or", "not" → And/Or/Not
//   - ==, ~=, <, >, <=, >= → Operator
//   - Quoted strings ("..." or '...') → Value
//   - Bare numbers (whole-word PJ::parseNumber, locale-independent) → Value
//   - ( → OpenParen, ) → CloseParen
//   - Everything else → Key
//
// Unrecognized characters are skipped (no crash, no infinite loop).
class Lexer {
 public:
  explicit Lexer(std::string_view source) : src_(source) {}

  [[nodiscard]] std::vector<Token> tokenize() const {
    std::vector<Token> tokens;
    int index = 0;
    int len = static_cast<int>(src_.size());

    while (index < len) {
      // Skip whitespace.
      if (isSpace(index)) {
        ++index;
        continue;
      }

      // Two-char operators.
      if (index + 1 < len) {
        auto two = slice(index, index + 2);
        if (two == "==" || two == "~=" || two == "<=" || two == ">=") {
          tokens.push_back({TokenType::kOperator, std::move(two), index, index + 2});
          index += 2;
          continue;
        }
      }

      // Single-char operators.
      if (at(index) == '<' || at(index) == '>') {
        tokens.push_back({TokenType::kOperator, std::string(1, at(index)), index, index + 1});
        ++index;
        continue;
      }

      // Parens.
      if (at(index) == '(') {
        tokens.push_back({TokenType::kOpenParen, "(", index, index + 1});
        ++index;
        continue;
      }
      if (at(index) == ')') {
        tokens.push_back({TokenType::kCloseParen, ")", index, index + 1});
        ++index;
        continue;
      }

      // Quoted string.
      if (at(index) == '"' || at(index) == '\'') {
        int start = index;
        char quote = at(index);
        ++index;
        while (index < len && at(index) != quote) {
          if (at(index) == '\\' && index + 1 < len) {
            ++index;  // skip escaped
          }
          ++index;
        }
        if (index < len) {
          ++index;  // consume closing quote
        }
        tokens.push_back({TokenType::kValue, slice(start, index), start, index});
        continue;
      }

      // Lone = or ~ (not part of a two-char op).
      if (at(index) == '=' || at(index) == '~') {
        tokens.push_back({TokenType::kOperator, std::string(1, at(index)), index, index + 1});
        ++index;
        continue;
      }

      // Word: identifier, number, or keyword.
      int start = index;
      while (index < len && !isSpace(index) && !isPunct(index)) {
        ++index;
      }

      if (index > start) {
        auto word = slice(start, index);
        tokens.push_back({classifyWord(word), std::move(word), start, index});
        continue;
      }

      // Unrecognized character — skip to prevent infinite loop.
      ++index;
    }

    return tokens;
  }

 private:
  // Bounds are managed by the callers; these only centralize the int → size_type
  // casts so -Wconversion stays clean.
  [[nodiscard]] char at(int index) const {
    return src_[static_cast<std::size_t>(index)];
  }

  [[nodiscard]] std::string slice(int from, int to) const {
    return std::string(src_.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
  }

  [[nodiscard]] bool isSpace(int index) const {
    return std::isspace(static_cast<unsigned char>(at(index)));
  }

  [[nodiscard]] bool isPunct(int index) const {
    char character = at(index);
    return character == '(' || character == ')' || character == '=' || character == '~' || character == '<' ||
           character == '>' || character == '"' || character == '\'';
  }

  [[nodiscard]] static TokenType classifyWord(const std::string& word) {
    if (word == "and") {
      return TokenType::kAnd;
    }
    if (word == "or") {
      return TokenType::kOr;
    }
    if (word == "not") {
      return TokenType::kNot;
    }

    // Whole-word number (locale-independent) → Value.
    if (PJ::parseNumber<double>(word)) {
      return TokenType::kValue;
    }

    return TokenType::kKey;
  }

  std::string_view src_;
};

}  // namespace PJ::query
