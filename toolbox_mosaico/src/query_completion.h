// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Context-aware completion mapping for the Mosaico MetadataQueryBar.
//
// PJ3's QueryBar::syncDropdowns drives three combos (key / operator / value)
// and an inline completer from complete()/analyze() at the live text-cursor
// position. The PJ4 dialog protocol has NO cursor-position channel — the only
// signal the host pushes is the editor text. We therefore treat the END of the
// current text as the cursor (the common "appending" case): every keystroke
// the user types lands at end-of-text, so end-of-text completion matches what
// the user is about to type next. This is a deliberate, documented limitation
// vs PJ3's full mid-text cursor sync; mid-text editing won't re-context the
// dropdowns until the caret returns to the end.
//
// This header maps the engine's complete()/analyze() output to the four
// protocol setters (keys / operators / values / flat completions). It is pure
// logic with no Qt dependency, so the mapping is unit-testable directly.

#pragma once

#include <string>
#include <vector>

#include "query/complete.h"
#include "query/edit.h"
#include "types.h"

namespace mosaico {

// What the query bar should advertise for the current end-of-text context.
struct CompletionContext {
  Expect expect = Expect::Any;  // what the engine thinks comes next
  std::string context_key;      // key in scope (for value completions), if any

  std::vector<std::string> keys;         // candidates for the Key combo
  std::vector<std::string> operators;    // candidates for the Operator combo
  std::vector<std::string> values;       // candidates for the Value combo (context_key's values)
  std::vector<std::string> completions;  // flat inline-completer list for the current context
};

// Compute the completion context for `query_text` with the cursor pinned to
// the end of the text (see file header for the end-of-text rationale).
//
// Mapping (matches complete()/analyze()):
//   - start / after connective / after open paren → keys
//   - after a key                                  → operators
//   - after an operator                            → that key's values
//   - after a value / close paren                  → connectives (and/or)
[[nodiscard]] inline CompletionContext computeCompletionContext(const std::string& query_text, const Schema& schema) {
  const std::size_t cursor = query_text.size();
  const auto comp = complete(query_text, cursor, schema);
  const auto ctx = analyze(query_text, static_cast<int>(cursor), schema);

  CompletionContext out;
  out.expect = comp.expect;
  out.context_key = comp.current_key;

  // The Key combo always lists the full key set (PJ3 keeps the key dropdown
  // populated; analyze() only toggles whether it is *enabled*). We still expose
  // the schema keys so the combo can be repopulated.
  for (const auto& [key, vals] : schema) {
    out.keys.push_back(key);
  }

  // Operators are the fixed engine set.
  out.operators = operators();

  // Values: the in-scope key's values, when we have one.
  if (!out.context_key.empty()) {
    if (auto it = schema.find(out.context_key); it != schema.end()) {
      out.values = it->second;
    }
  }

  // The inline-completer list reflects exactly what the engine suggests next
  // in this context (keys at start, operators after a key, that key's values
  // after an operator, connectives after a value).
  out.completions = comp.suggestions;

  return out;
}

}  // namespace mosaico
