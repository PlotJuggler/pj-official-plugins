/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <string_view>

#include "edit.h"   // analyze, applyCompletion, quoteValueForQuery, Action, CursorContext, is_query_whitespace
#include "types.h"  // Schema

// Assist-UI model for the Mosaico metadata-filter Key/Op/Value dropdowns + the
// PLUS button. Pure / Qt-free so it is unit-testable; the plugin
// (mosaico_dialog.cpp) drives the real combos + Lua editor from it.
//
// Contract (lua.txt):
//   REPLACE — when the caret is ON a key/op/value token, that ONE dropdown is
//     active and a pick REPLACES the token in place, live, with NO PLUS. Its
//     title syncs to the token under the caret.                 (lua.txt 4,5,6,7,8)
//   ADD — when the caret is NOT on a token (end / new clause), the dropdowns STAGE
//     a clause: pick a Key (the Key dropdown then shows it and disables, focus
//     moves to Op), Op auto-shows "==" (overridable), pick a Value; the clause is
//     inserted into the editor ONLY when PLUS is pressed, joined with " and " if
//     the editor already holds a filter. PLUS is the ONLY way a NEW clause enters
//     the text.                                                 (lua.txt 1,2,3,9)

// The operator auto-selected while staging a new clause (lua.txt 2).
inline constexpr const char* kDefaultOp = "==";

// State of one assist dropdown.
struct AssistDropdown {
  bool enabled = false;
  // true  : a pick REPLACES the token under the caret (live, no PLUS).
  // false : a pick STAGES this slot for the next PLUS (only meaningful when enabled).
  bool replaces = false;
  // Text shown as the dropdown's main label ("" == placeholder).
  std::string title;
};

// Full assist state for a (text, caret, staged-clause) snapshot.
struct AssistView {
  AssistDropdown key;
  AssistDropdown op;
  AssistDropdown value;
  // The metadata key whose distinct values populate the Value dropdown — the
  // staged key in ADD mode, or the clause's key in value-REPLACE mode.
  std::string value_key;
  // PLUS can commit the staged clause (a Key + a Value are staged; Op defaults to
  // "=="). Always false in REPLACE mode — replacements never use PLUS.
  bool plus_enabled = false;
};

// Compute the dropdown + PLUS state. `staged_*` describe the clause being built in
// ADD mode (empty == not yet picked); they are ignored in REPLACE mode.
[[nodiscard]] inline AssistView computeAssist(
    std::string_view text, int caret, const Schema& schema, std::string_view staged_key, std::string_view staged_op,
    std::string_view staged_value) {
  const CursorContext ctx = analyze(text, caret, schema);
  AssistView v;

  // REPLACE mode: the caret sits on a token, so its dropdown edits it in place and
  // syncs its title to the token text (lua.txt 4-8). The other two stay inert; no PLUS.
  // A Value is replaceable only STRICTLY inside it: the caret at a value's trailing
  // edge ends the clause, which is a chain (ADD) position, not a value edit — this is
  // exactly the spot the caret lands on after PLUS.
  if (ctx.key_action == Action::Replace) {
    v.key = {true, true, ctx.active_token.text};
    return v;
  }
  if (ctx.op_action == Action::Replace) {
    v.op = {true, true, ctx.active_token.text};
    return v;
  }
  if (ctx.val_action == Action::Replace && caret < ctx.active_token.end) {
    v.value = {true, true, ctx.active_token.text};
    v.value_key = ctx.context_key;
    return v;
  }

  // ADD mode: stage a new clause (lua.txt 1-3). Key is active until one is picked,
  // then it shows the key and disables while focus moves to Op; Op + Value activate
  // once a Key is staged, with Op pre-showing "==" (overridable, lua.txt 2). PLUS
  // lights up once a Value is staged (lua.txt 3).
  const bool have_key = !staged_key.empty();
  const bool have_value = !staged_value.empty();
  const std::string op_title = staged_op.empty() ? std::string(kDefaultOp) : std::string(staged_op);

  v.key = {/*enabled=*/!have_key, /*replaces=*/false, /*title=*/std::string(staged_key)};
  v.op = {/*enabled=*/have_key, /*replaces=*/false, /*title=*/have_key ? op_title : std::string()};
  v.value = {/*enabled=*/have_key, /*replaces=*/false, /*title=*/std::string(staged_value)};
  v.value_key = std::string(staged_key);
  v.plus_enabled = have_key && have_value;
  return v;
}

// PLUS commit (lua.txt 3, 9): build `key op "value"` (value quoted as a Lua literal)
// and append it to `existing`. If `existing` holds any non-whitespace content, join
// with the default ADD operator " and "; otherwise the clause stands alone.
[[nodiscard]] inline std::string commitClause(
    std::string_view existing, std::string_view key, std::string_view op, std::string_view value) {
  std::string clause;
  clause.reserve(key.size() + op.size() + value.size() + 8);
  clause.append(key);
  clause.push_back(' ');
  clause.append(op);
  clause.push_back(' ');
  clause.append(quoteValueForQuery(value));

  bool has_existing = false;
  for (const char c : existing) {
    if (!is_query_whitespace(c)) {
      has_existing = true;
      break;
    }
  }
  if (!has_existing) {
    return clause;
  }
  std::string out(existing);
  while (!out.empty() && is_query_whitespace(out.back())) {
    out.pop_back();
  }
  out.append(" and ");
  out.append(clause);
  return out;
}
