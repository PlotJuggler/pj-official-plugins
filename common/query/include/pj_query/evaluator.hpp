// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#pragma once

#include "pj_query/query.hpp"
#include "pj_query/types.hpp"

namespace PJ::query {

struct ValidationResult {
  bool valid = false;
  std::string error;
  int line = 0;
  int column = 0;
};

/// Plugin-implemented seam for expression validation/evaluation. A Lua adapter
/// owns its state locally, validates the raw text (after its chosen expansion),
/// evaluates Query::expanded(), and clears metadata between rows. Query::complete
/// describes the editing grammar; it is not authoritative evaluator validation.
class Evaluator {
 public:
  virtual ~Evaluator() = default;
  [[nodiscard]] virtual ValidationResult validate(std::string_view text) = 0;
  [[nodiscard]] virtual bool evaluate(const Query& query, const Metadata& metadata) = 0;
};

}  // namespace PJ::query
