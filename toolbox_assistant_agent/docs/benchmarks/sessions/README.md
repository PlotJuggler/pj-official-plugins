# Archived turns

Raw session records written by the Claude Code CLI, one JSON object per line,
copied here unedited. Read them with `../session_report.py`.

They are kept because they are the only complete account of what a turn cost and
why. The panel shows what the CLI's final `result` record reports; these files
carry the `usage` and `timestamp` of every individual request, plus the tool
sequence and the model's own reasoning blocks. The two do not agree — the panel
read 371k for the reference turn where the per-request sum is 1.67 M — and until
that is explained these files are the ones to trust, because they are the sum of
what each request actually reported rather than one aggregate at the end.

Everything measured about this plugin's turn cost came out of here:

- The CLI never puts two tools in one response, so **every tool call is a full
  round trip** that re-sends the whole conversation.
- **Consecutive runs of one tool are 62% of the reference turn.** That is what
  batched reads exist to collapse.
- **Late rounds cost far more than early ones** — 21,648 tokens on average in the
  first half of the turn against 58,816 in the second — because each one carries
  everything said so far. Saving a round near the end is worth several near the
  start, which is the opposite of where the obvious optimisation would go.

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
| `2026-08-11-nissan-reference.jsonl` | The turn measured before batched reads: 41 round trips, 31 tool calls, 1.67 M tokens, 247 s. Every comparison is against this. |
