# Plugin Porting: Behavioral Rules

Porting is **mechanical translation**, not rewriting. The original plugin is the specification: the
port must behave identically in every observable way, using the new SDK as the target language.
Read `porting_guide.md` (repository root) in its entirety before starting; it covers SDK patterns,
datastore pitfalls, dialog SDK details and known issues.

## Mandatory workflow

1. Read the original code line by line before writing anything: every function, signal/slot
   connection and conditional branch.
2. Read `porting_guide.md`. Do not start coding until you have read both.

## Rules

1. **Every code path in the original has a corresponding code path in the port.** 14 branches in
   the original means 14 in the port; a warning on a condition is emitted on the same condition.
2. **There are no minor features.** Dialog geometry, splitter ratios, help buttons, column
   selection history, cosmetic labels: every feature the user can observe or interact with is
   mandatory. Classify features only as "ported" or "not yet ported".
3. **Do not optimize, simplify, or improve the original's behavior.** Translate verbose or
   roundabout logic faithfully. If you believe something is a bug, flag it; do not silently fix it.
4. **Do not substitute a different approach.** Use the same widget, data structure or algorithm;
   "close enough" is not acceptable.
5. **Every signal/slot connection in the original dialog is replicated.** If the original wires
   `itemDoubleClicked` to `accept()`, so must the port.
6. **Speed does not matter. Completeness does.** Multiple sessions per plugin is expected. Never
   declare a plugin "ported" when features have been silently dropped.

## When you cannot translate something

If the SDK lacks a capability the original uses, either **extend the SDK** to support it, or
**stop and ask the user**. Never silently drop the feature, substitute a simpler alternative, or log
it as a "minor gap".

## Verification before claiming done

Produce a **feature audit table** with one row per feature of the original:

| # | Original Feature | Original Code Location | Status | New Code Location | Notes |
|---|-----------------|----------------------|--------|-------------------|-------|

Every row must be filled. The port is done only when every row says DONE or BLOCKED (with an
explicit SDK limitation and a user decision).

## Failure mode red flags

If you catch yourself thinking any of these, stop and correct:

- **"This is just cosmetic"**: cosmetic features are features. Port them.
- **"The SDK doesn't support X, so I'll use Y instead"**: stop and ask.
- **"I'll come back to this later"**: port it now or mark BLOCKED with a specific SDK limitation.
- **"The original code is messy, let me clean it up"**: translate as-is; cleanup is a separate task
  after verified correctness.
- **"This edge case probably never happens"**: the original author handled it; so do you.
- **"I'm almost done, just these last few things"**: run the audit table. Any row not DONE or
  BLOCKED means you are not almost done.
