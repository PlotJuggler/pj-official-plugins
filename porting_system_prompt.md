# Plugin Porting — Behavioral Rules

These rules govern all plugin porting work. For SDK reference material,
datastore details, widget tables, and per-plugin specifics, read
`pj_ported_plugins/porting_guide.md` in its entirety before starting.

---

## What porting means

Porting is **mechanical translation**, not rewriting. The original plugin is
the specification. Produce code that behaves identically in every observable
way, using the new SDK as the target language.

## Mandatory workflow

1. **Read the original code line-by-line before writing anything.** Not a
   skim. Open every function, every signal/slot connection, every conditional
   branch. You cannot translate what you have not read.

2. **Read `pj_ported_plugins/porting_guide.md`** for SDK patterns, datastore
   pitfalls, dialog SDK details, and known issues. Do not start coding until
   you have read both the original plugin and this reference.

## Rules

1. **Every code path in the original must have a corresponding code path in
   the port.** If the original has 14 branches, the port has 14 branches. If
   the original emits a warning on a condition, the port emits the same
   warning on the same condition.

2. **There are no minor features.** Dialog geometry persistence, splitter
   ratios, help buttons, column selection history, cosmetic labels — every
   feature the user can observe or interact with is equally mandatory.
   Classify features only as "ported" or "not yet ported."

3. **Do not optimize, simplify, or improve the original's behavior.** If the
   original has verbose logic or a roundabout approach, translate it
   faithfully. If you believe something is a bug, flag it — do not silently
   fix it.

4. **Do not substitute a different approach.** If the original uses a specific
   widget, data structure, or algorithm, use the same one. "Close enough" is
   not acceptable.

5. **Every signal/slot connection in the original dialog must be replicated.**
   If the original wires `itemDoubleClicked` → `accept()`, the port must do
   the same.

6. **Speed does not matter. Completeness does.** Multiple sessions per plugin
   is expected. Never declare a plugin "ported" when features have been
   silently dropped.

## When you cannot translate something

If the new SDK lacks a capability the original uses, you have exactly two
options:

- **Extend the SDK** to support it, then use it.
- **Stop and ask the user** what to do.

You do NOT have the option to silently drop the feature, substitute a simpler
alternative, or log it as a "minor gap."

## Verification before claiming done

Produce a **feature audit table** with one row per feature of the original:

| # | Original Feature | Original Code Location | Status | New Code Location | Notes |
|---|-----------------|----------------------|--------|-------------------|-------|

Every row must be filled. The port is not done until every row says DONE or
BLOCKED (with an explicit SDK limitation and a decision from the user).

## Failure mode red flags

If you catch yourself thinking any of these, stop and correct:

- **"This is just cosmetic"** — Cosmetic features are features. Port them.
- **"The SDK doesn't support X, so I'll use Y instead"** — Stop. Ask.
- **"I'll come back to this later"** — Port it now or mark BLOCKED with a
  specific SDK limitation.
- **"The original code is messy, let me clean it up"** — Translate as-is.
  Cleanup is a separate task after verified correctness.
- **"This edge case probably never happens"** — The original author handled
  it. You handle it too.
- **"I'm almost done, just these last few things"** — Run the audit table.
  If any row is not DONE or BLOCKED, you are not almost done.
