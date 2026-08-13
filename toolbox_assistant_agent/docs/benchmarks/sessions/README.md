# Archived turns

Raw session records written by the Claude Code CLI, one JSON object per line,
copied here unedited. Read them with `../session_report.py`.

They are kept because they are the only complete account of what a turn cost and
why. The panel shows what the CLI's final `result` record reports; these files
carry the `usage` and `timestamp` of every individual request, plus the tool
sequence and the model's own reasoning blocks.

Everything measured about this plugin's turn cost came out of here:

- The CLI never puts two tools in one response, so **every tool call is a full
  round trip** that re-sends the whole conversation.
- **Almost nothing is recomputed.** Across the 41 rounds of the reference turn,
  82 tokens were fresh input; 1.12 M were cache reads and 548 k cache writes. The
  conversation really is re-sent every round, and served from cache.
- **Cached is not free, and not full price either.** Reads bill at 0.1x a fresh
  input token, writes at 1.25x. Priced that way the reference turn is 797 k
  equivalent tokens rather than the 1.67 M of raw volume.
- **Consecutive runs of one tool are 68% of the reference turn.** That is what
  batched reads exist to collapse.

### A claim that did not survive being priced

An earlier version of this file said late rounds cost far more than early ones —
21,648 tokens on average in the first half of the turn against 58,816 in the
second — and concluded that saving a round near the end was worth several near
the start.

That was an artefact of counting cached tokens at full price. Weighted, the two
halves are 18,909 and 19,931: **flat**. A longer conversation does make each
round carry more text, but nearly all of the extra is cache READ at a tenth
price. What a round adds to the bill is roughly what it newly writes, and that
stays about constant.

So a round costs about a round, wherever it falls — a simpler rule than the one
it replaces, and it means collapsing a run of fifteen early reads is worth
exactly what it looks like it is worth.

The panel still disagrees with the per-request sum (371k against 797k weighted,
1.67 M raw). Weighting closes about half of that gap; the rest is unexplained, so
these files remain the ones to trust.

## Reading a run

    python3 ../session_report.py <file>.jsonl

Two things worth checking before believing any number:

- `model=<synthetic>` with zero tokens is not a fast turn, it is an error turn.
  A rejected model name produces one, and it looks exactly like a large speedup.
- The prompt must be identical across compared runs. The screenshots taken by
  the GUI driver record what was actually typed, because an automated keystroke
  can drop characters silently.

## What is here

| file | what it is |
|---|---|
| `2026-08-11-nissan-reference.jsonl` | Before batched reads: 41 round trips, 31 tool calls, 797 k equivalent tokens (1.67 M raw), 247 s. Every comparison is against this. |
| `2026-08-12-nissan-sonnet-batched.jsonl` | Same prompt, same four series and marker set, after batching: 22 round trips, 10 tool calls, 259 k equivalent, 191 s. |
| `2026-08-12-nissan-{haiku,opus,fable}-batched.jsonl` | The same prompt on the other three tiers. They did **not** do the same work — 13, 30 and 10 derived series against Sonnet's 4 — so these compare choices, not prices. |
