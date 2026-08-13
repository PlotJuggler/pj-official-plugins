#!/usr/bin/env python3
"""Summarize what a turn actually cost, from the CLI's own session record.

The Claude Code CLI writes every turn to ~/.claude/projects/<slug>/<id>.jsonl,
one JSON object per line, with `usage` and `timestamp` on each assistant
message. That file is a far better instrument than anything this plugin could
add: it already carries round trips, per-message token counts, wall-clock gaps,
tool calls and the model's own reasoning blocks.

It answers the question the panel cannot. The status bar reports what the CLI's
final `result` record says, which does not agree with the sum of the per-message
usage in the same session — by a factor of about 4.5 in the reference turn. Until
that is explained, this is the number to trust: it is the sum of what each
request actually reported.

What matters here is CONSECUTIVE RUNS of the same tool. The CLI never puts two
tools in one response, so every call is a full round trip that re-sends the whole
conversation. A run of fifteen reads is fifteen of those, and collapsing runs is
the single largest lever measured on this plugin.

Usage:
    session_report.py <session.jsonl> [<session.jsonl> ...]
"""

import json
import sys
from datetime import datetime


def _time(stamp):
    return datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def _sent(usage):
    """Everything that went up the wire: fresh prompt plus replayed cache.

    `input_tokens` alone is misleading — in a resumed conversation it reads ~10
    while cache_read reads tens of thousands. The conversation IS being sent; it
    is just served from cache.
    """
    return (
        usage.get("input_tokens", 0)
        + usage.get("cache_read_input_tokens", 0)
        + usage.get("cache_creation_input_tokens", 0)
    )


def _tool_of(row):
    for block in row.get("message", {}).get("content", []) or []:
        if isinstance(block, dict) and block.get("type") == "tool_use":
            return block.get("name", "").replace("mcp__pj__", "")
    return None


def _runs(tools):
    """Consecutive stretches of the same tool, as (name, [indices])."""
    out, cur = [], []
    for i, name in enumerate(tools):
        if name is None:
            if len(cur) >= 2:
                out.append((tools[cur[0]], cur))
            cur = []
        elif cur and tools[cur[0]] == name:
            cur.append(i)
        else:
            if len(cur) >= 2:
                out.append((tools[cur[0]], cur))
            cur = [i]
    if len(cur) >= 2:
        out.append((tools[cur[0]], cur))
    return out


def report(path):
    rows = [json.loads(line) for line in open(path) if line.strip()]
    turns = [r for r in rows if r.get("type") == "assistant"]
    if not turns:
        print(f"{path}: no assistant messages")
        return

    usages = [t["message"].get("usage", {}) for t in turns]
    sent = [_sent(u) for u in usages]
    made = [u.get("output_tokens", 0) for u in usages]
    tools = [_tool_of(t) for t in turns]
    stamps = [_time(t["timestamp"]) for t in turns]
    model = turns[0]["message"].get("model", "?")
    thinking = sum(
        1
        for t in turns
        for b in t["message"].get("content", []) or []
        if isinstance(b, dict) and b.get("type") == "thinking"
    )

    total = sum(sent)
    print(f"\n=== {path.split('/')[-1][:8]}  model={model} ===")
    print(f"  round trips     : {len(turns)}")
    print(f"  tool calls      : {sum(1 for x in tools if x)}")
    print(f"  wall clock      : {(stamps[-1] - stamps[0]).total_seconds():.0f} s")
    print(f"  sent / generated: {total:,} / {sum(made):,} tokens")
    print(f"  reasoning blocks: {thinking}")
    if len(turns) > 1:
        print(f"  cost of a round : {total / len(turns):,.0f} tokens on average")
        # Late rounds carry the whole accumulated conversation, so they cost
        # several times what early ones do. Collapsing a run late in a turn is
        # worth more than collapsing a longer one at the start.
        half = len(turns) // 2
        print(
            f"    first half    : {sum(sent[:half]) / max(half, 1):,.0f}"
            f"   second half: {sum(sent[half:]) / max(len(turns) - half, 1):,.0f}"
        )

    runs = _runs(tools)
    if runs:
        print("  consecutive runs of one tool (what a batch collapses):")
        collapsible = 0
        for name, idx in runs:
            tk = sum(sent[i] for i in idx)
            collapsible += tk
            print(f"    {len(idx):>3} x {name:<22} rounds {idx[0]+1}-{idx[-1]+1}  {tk:>10,} tokens")
        print(f"    -> {collapsible:,} tokens = {collapsible / total * 100:.0f}% of the turn")
    else:
        print("  consecutive runs: none — every call stands alone")

    used = {}
    for name in tools:
        if name:
            used[name] = used.get(name, 0) + 1
    if used:
        print("  tools used: " + ", ".join(f"{k}x{v}" for k, v in sorted(used.items())))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(2)
    for arg in sys.argv[1:]:
        report(arg)
