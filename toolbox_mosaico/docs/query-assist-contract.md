<!-- SPDX-License-Identifier: MPL-2.0 -->

# Mosaico query-assist contract

The behaviour contract for the metadata-filter **Key / Op / Value** dropdowns and
the **PLUS** button in the Mosaico toolbox dialog.

The model lives in `src/query/assist.h` (Qt-free, unit-testable);
`src/mosaico_dialog.cpp` drives the real combo boxes and the Lua editor from it.
`tests/assist_test.cpp` pins each clause below with one test.

The contract has two modes, chosen by where the caret sits.

## REPLACE — caret is **on** a key/op/value token

Exactly one dropdown is active, and picking from it replaces that token **in
place, live, with no PLUS**. Its title syncs to the token under the caret.

| # | Clause |
|---|---|
| 4 | Caret on a **key** activates the Key dropdown for replacement; the other two are inert. |
| 5 | Caret on an **operator** activates the Op dropdown for replacement. |
| 6 | Caret on a **value** activates the Value dropdown for replacement. |
| 7 | Replacements happen without PLUS (PLUS stays disabled in REPLACE mode), and the edit is in place (`applyCompletion` `Replace`). |
| 8 | The user may type the next token; the dropdowns synchronise to the token under the caret — a freely typed key reflects in the Key dropdown. |

## ADD — caret is **not** on a token (end of text, or a new clause)

The dropdowns **stage** a clause rather than editing the text. The staged clause
enters the editor only when PLUS is pressed.

| # | Clause |
|---|---|
| 1 | On key select, the Key dropdown shows that key as its title and becomes disabled as focus moves on to the Op dropdown. |
| 2 | When building at the end, the Op auto-selects `==`, and stays enabled so the user can override it. |
| 3 | A selected value only enters the editor on PLUS; PLUS joins with the default ADD operator `" and "` when a filter is already present. |
| 9 | PLUS is the only entry that inserts a **new** clause: in ADD mode the dropdowns stage and never replace or edit the text; only `commitClause` writes. |

## Summary

REPLACE edits at the caret and never uses PLUS. ADD stages, and PLUS is the only
path by which a new clause reaches the editor.
