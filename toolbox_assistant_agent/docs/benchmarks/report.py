#!/usr/bin/env python3
"""Render BENCHMARKS.md tables from a raw matrix run.

The tables in BENCHMARKS.md are generated, never hand-typed: a number copied by
hand is a number nobody can re-derive, and this study is meant to be re-run when
models change.

    python3 docs/benchmarks/report.py docs/benchmarks/data/<run>.json
"""
import json
import statistics
import sys
from collections import defaultdict

# Ascending difficulty. The interesting question is where each tier stops.
# L9+ are the hard tier, added after L1-L8 turned out to be cleared by every
# model on every repetition — a suite nobody fails locates no ceiling.
ORDER = ["L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "L10", "L11", "L12"]


def load(path):
    with open(path) as f:
        return json.load(f)


def by(cells, *keys):
    out = defaultdict(list)
    for c in cells:
        out[tuple(c[k] for k in keys)].append(c)
    return out


def med(values):
    return statistics.median(values) if values else float("nan")


def pass_matrix(cells, models, titles):
    """Success rate per model x scenario — the capability ceiling."""
    grouped = by(cells, "model", "scenario")
    lines = ["| Scenario | " + " | ".join(models) + " |",
             "|---" * (len(models) + 1) + "|"]
    for sid in ORDER:
        if not any((m, sid) in grouped for m in models):
            continue
        row = [f"**{sid}** {titles.get(sid, '')}"]
        for m in models:
            runs = grouped.get((m, sid), [])
            if not runs:
                row.append("—")
                continue
            ok = sum(1 for r in runs if r["pass"])
            mark = "✅" if ok == len(runs) else ("⚠️" if ok else "❌")
            row.append(f"{mark} {ok}/{len(runs)}")
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def timing_matrix(cells, models, titles):
    grouped = by(cells, "model", "scenario")
    lines = ["| Scenario | " + " | ".join(models) + " |",
             "|---" * (len(models) + 1) + "|"]
    for sid in ORDER:
        if not any((m, sid) in grouped for m in models):
            continue
        row = [f"**{sid}** {titles.get(sid, '')}"]
        for m in models:
            runs = grouped.get((m, sid), [])
            row.append(f"{med([r['wall_s'] for r in runs]):.1f} s" if runs else "—")
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def summary(cells, models):
    grouped = by(cells, "model")
    lines = ["| Model | Passed | Median turn | Median round-trips | Mean cost | Mean output tokens |",
             "|---|---|---|---|---|---|"]
    for m in models:
        runs = grouped.get((m,), [])
        if not runs:
            continue
        ok = sum(1 for r in runs if r["pass"])
        lines.append(
            f"| `{m}` | {ok}/{len(runs)} ({100.0 * ok / len(runs):.0f} %) "
            f"| {med([r['wall_s'] for r in runs]):.1f} s "
            f"| {med([r['round_trips'] for r in runs]):.0f} "
            f"| ${statistics.mean([r['cost_usd'] for r in runs]):.3f} "
            f"| {statistics.mean([r['output_tokens'] for r in runs]):.0f} |")
    return "\n".join(lines)


def ceilings(cells, models):
    """Highest scenario each model passes on EVERY repetition, and where it first breaks."""
    grouped = by(cells, "model", "scenario")
    lines = ["| Model | Clears without a miss | First scenario it misses |", "|---|---|---|"]
    for m in models:
        clean, first_miss = [], None
        for sid in ORDER:
            runs = grouped.get((m, sid), [])
            if not runs:
                continue
            if all(r["pass"] for r in runs):
                clean.append(sid)
            elif first_miss is None:
                first_miss = f"{sid} ({sum(1 for r in runs if r['pass'])}/{len(runs)})"
        lines.append(f"| `{m}` | {', '.join(clean) if clean else '—'} | {first_miss or 'none'} |")
    return "\n".join(lines)


def failures(cells):
    out = []
    for c in cells:
        if not c["pass"]:
            out.append(f"- **{c['model']} {c['scenario']}** rep{c['rep']}: {c['failure']}")
    return "\n".join(out) if out else "_No scenario failed on any model._"


def main():
    cells = load(sys.argv[1])
    models, titles = [], {}
    for c in cells:
        if c["model"] not in models:
            models.append(c["model"])
        titles[c["scenario"]] = c["title"]

    total = sum(c["cost_usd"] for c in cells)
    print(f"Cells: {len(cells)}   Models: {', '.join(models)}   Total spend: ${total:.2f}\n")
    print("## Capability\n");   print(pass_matrix(cells, models, titles))
    print("\n## Where each tier stops\n"); print(ceilings(cells, models))
    print("\n## Median turn time\n");  print(timing_matrix(cells, models, titles))
    print("\n## Per model\n");    print(summary(cells, models))
    print("\n## Every failure\n");  print(failures(cells))


if __name__ == "__main__":
    main()
