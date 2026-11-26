// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "pj_query/token.hpp"
#include "pj_query/types.hpp"

namespace PJ::query {

enum class Expect {
  kKey,         // a metadata key name
  kOperator,    // ==, ~=, <, >, <=, >=
  kValue,       // a value for the current key
  kConnective,  // and, or, not
  kAny,         // beginning of expression or after open paren
};

struct Completions {
  Expect expect = Expect::kAny;
  std::string current_key;               // the key in context (for Value completions)
  std::vector<std::string> suggestions;  // valid items to insert
};

inline const std::vector<std::string>& operators() {
  static const std::vector<std::string> ops = {"==", "~=", "<", ">", "<=", ">="};
  return ops;
}

inline const std::vector<std::string>& connectives() {
  static const std::vector<std::string> conns = {"and", "or", "not"};
  return conns;
}

// Computes completions from the tokens preceding the cursor (the caller slices the Lexer's token
// stream; this never re-scans bytes).
//
// `last_token_complete` distinguishes the two callers: complete(text, cursor) truncates the text
// at the cursor, so a trailing word may be a partial key still being typed (filter schema keys by
// prefix); analyze() passes the FULL token the cursor sits on, so a trailing key means "an
// operator goes next" even when the key is not in the schema.
[[nodiscard]] inline Completions complete(
    const std::vector<Token>& tokens, const Schema& schema, bool last_token_complete = false) {
  Completions result;

  if (tokens.empty()) {
    result.expect = Expect::kAny;
    for (const auto& [key, vals] : schema) {
      result.suggestions.push_back(key);
    }
    return result;
  }

  const auto& last = tokens.back();

  if (last.type == TokenType::kAnd || last.type == TokenType::kOr || last.type == TokenType::kNot ||
      last.type == TokenType::kOpenParen) {
    result.expect = Expect::kKey;
    for (const auto& [key, vals] : schema) {
      result.suggestions.push_back(key);
    }
    return result;
  }

  if (last.type == TokenType::kOperator) {
    result.expect = Expect::kValue;
    if (tokens.size() >= 2) {
      result.current_key = tokens[tokens.size() - 2].text;
      auto it = schema.find(result.current_key);
      if (it != schema.end()) {
        result.suggestions = it->second;
      }
    }
    return result;
  }

  // After key-op-value or a closing paren — expect a connective.
  if (tokens.size() >= 3 && tokens[tokens.size() - 2].type == TokenType::kOperator) {
    result.expect = Expect::kConnective;
    result.suggestions = {"and", "or"};
    if (tokens[tokens.size() - 3].type == TokenType::kKey) {
      result.current_key = tokens[tokens.size() - 3].text;
    }
    return result;
  }

  if (last.type == TokenType::kCloseParen || (last.type == TokenType::kValue && last_token_complete)) {
    result.expect = Expect::kConnective;
    result.suggestions = {"and", "or"};
    return result;
  }

  // A trailing key (or a bare value with nothing to attach it to).
  if (last_token_complete) {
    result.expect = Expect::kOperator;
    result.current_key = last.text;
    result.suggestions = operators();
    return result;
  }
  auto it = schema.find(last.text);
  if (it != schema.end()) {
    result.expect = Expect::kOperator;
    result.current_key = last.text;
    result.suggestions = operators();
    return result;
  }

  // Partial key — filter keys by prefix.
  result.expect = Expect::kKey;
  for (const auto& [key, vals] : schema) {
    if (key.compare(0, last.text.size(), last.text) == 0) {
      result.suggestions.push_back(key);
    }
  }
  return result;
}

// The cursor is a character offset into `text` (the end of the text for appending).
[[nodiscard]] inline Completions complete(std::string_view text, std::size_t cursor, const Schema& schema) {
  const auto prefix = text.substr(0, std::min(cursor, text.size()));
  return complete(Lexer(prefix).tokenize(), schema);
}

}  // namespace PJ::query
