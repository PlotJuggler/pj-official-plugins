#!/usr/bin/env python3
"""Aggregate report over one run directory of the real-data benchmark.

Usage: realdata_report.py runs/<date>/ > report.md

Reads what `bench_cli.py` left per turn (metrics.json, `done`, ABORT.txt) and
what `verify.py` left per cell (`score_<task>.json`, falling back to the bare
`score.json` -- see verify.py's `main()` for why both exist) under each
`runs/<date>/<cell-id>/` directory, and prints a Markdown report to stdout:

  1. a pass matrix: task/file (rows) x model/arm (columns), each cell either
     a pass mark, a fail mark with the first failing required check's reason,
     "not run" (no `done` file, or an ABORT.txt), or "not scored" (ran, but
     no score_<task>.json/score.json next to it -- run verify.py first).
  2. per-model turn statistics: median wall seconds, median assistant
     messages (= round trips), median tool calls, tool-call counts by name,
     and token totals + medians for input/output/cache_read/cache_creation
     exactly as the CLI reported them in metrics.json -- no dollar figures
     anywhere, by design (see the task brief this was built from).
  3. a per-cell table of the numeric errors pulled out of each score's
     `computed` section (best_err / dt / iou / matched-missed-spurious, per
     task).

Python 3 stdlib only.
"""
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify  # noqa: E402 -- reuse parse_cell_dir, no re-implementation


TOKEN_KEYS = ["input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"]
TOKEN_LABELS = {
    "input_tokens": "input",
    "output_tokens": "output",
    "cache_read_input_tokens": "cache_read",
    "cache_creation_input_tokens": "cache_creation",
}


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------


def _looks_like_cell_dir(path):
    if not path.is_dir():
        return False
    if (path / "cell.json").is_file():
        return True
    if any(p.is_dir() and p.name.startswith("turn") for p in path.iterdir()):
        return True
    return False


def discover_cells(run_dir):
    run_dir = Path(run_dir)
    if not run_dir.is_dir():
        return []
    return sorted(p for p in run_dir.iterdir() if _looks_like_cell_dir(p))


def _load_json(path):
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def load_score(cell_dir, task):
    """`score_<task>.json` first (collision-proof for multi-turn cells),
    falling back to the bare `score.json` for a single-task cell scored
    before the qualified file existed."""
    return _load_json(cell_dir / f"score_{task}.json") or _load_json(cell_dir / "score.json")


class TurnRecord:
    __slots__ = ("cell_dir", "cell_id", "dataset", "file", "model", "arm", "task", "turn_no", "turn_dir", "status", "score", "metrics")

    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


def collect_turns(run_dir):
    """One TurnRecord per (cell, task) pair found under `run_dir`."""
    records = []
    for cell_dir in discover_cells(run_dir):
        parsed = verify.parse_cell_dir(cell_dir)
        turns = parsed["turns"]
        if not turns:
            continue
        for i, task in enumerate(turns, start=1):
            turn_dir = cell_dir / f"turn{i}"
            done_path = turn_dir / "done"
            abort_path = turn_dir / "ABORT.txt"
            if abort_path.is_file():
                status = "not_run"
            elif not done_path.is_file():
                status = "not_run"
            else:
                status = "ran"
            score = load_score(cell_dir, task) if status == "ran" else None
            metrics = _load_json(turn_dir / "metrics.json") if status == "ran" else None
            records.append(
                TurnRecord(
                    cell_dir=cell_dir,
                    cell_id=cell_dir.name,
                    dataset=parsed["dataset"],
                    file=parsed["file"],
                    model=parsed["model"],
                    arm=parsed["arm"],
                    task=task,
                    turn_no=i,
                    turn_dir=turn_dir,
                    status=status,
                    score=score,
                    metrics=metrics,
                )
            )
    return records


# ---------------------------------------------------------------------------
# 1. Pass matrix
# ---------------------------------------------------------------------------


def _first_failed_required_reason(score):
    for name in score.get("required", []):
        check = score.get("checks", {}).get(name, {})
        if not check.get("pass", False):
            return f"{name}: {check.get('reason', '')}"
    return ""


def render_pass_matrix(records):
    lines = ["## Pass matrix\n"]
    rows_order = []
    cols_order = []
    cell_text = {}
    for r in records:
        row_key = (r.dataset, r.file, r.task)
        col_key = f"{r.model}/{r.arm}"
        if row_key not in rows_order:
            rows_order.append(row_key)
        if col_key not in cols_order:
            cols_order.append(col_key)
        if r.status == "not_run":
            text = "not run"
        elif r.score is None:
            text = "not scored"
        elif r.score.get("pass"):
            text = "✅"
        else:
            reason = _first_failed_required_reason(r.score)
            text = f"❌ {reason}" if reason else "❌"
        cell_text[(row_key, col_key)] = text

    header = "| task/file | " + " | ".join(cols_order) + " |"
    sep = "|---" * (len(cols_order) + 1) + "|"
    lines.append(header)
    lines.append(sep)
    for row_key in rows_order:
        dataset, file_id, task = row_key
        label = f"{task} {dataset}/{file_id}"
        cells = [cell_text.get((row_key, c), "") for c in cols_order]
        lines.append(f"| {label} | " + " | ".join(cells) + " |")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# 2. Per-model turn statistics
# ---------------------------------------------------------------------------


def render_model_stats(records):
    lines = ["## Per-model turn statistics\n"]
    by_model = {}
    for r in records:
        if r.status != "ran" or not r.metrics:
            continue
        by_model.setdefault(r.model, []).append(r)

    for model in sorted(by_model):
        turns = by_model[model]
        wall_s = [t.metrics.get("wall_s") for t in turns if isinstance(t.metrics.get("wall_s"), (int, float))]
        n_msgs = [t.metrics.get("assistant_message_count") for t in turns if isinstance(t.metrics.get("assistant_message_count"), (int, float))]
        tool_counts_per_turn = []
        tool_totals = {}
        for t in turns:
            counts = t.metrics.get("tool_use_counts", {}) or {}
            tool_counts_per_turn.append(sum(counts.values()))
            for name, c in counts.items():
                tool_totals[name] = tool_totals.get(name, 0) + c

        token_values = {k: [] for k in TOKEN_KEYS}
        for t in turns:
            usage = (t.metrics.get("result") or {}).get("usage") or {}
            for k in TOKEN_KEYS:
                v = usage.get(k)
                if isinstance(v, (int, float)):
                    token_values[k].append(v)

        lines.append(f"### {model} ({len(turns)} turn(s))\n")
        lines.append(f"- median wall seconds/turn: {_median(wall_s)}")
        lines.append(f"- median assistant messages/turn (round trips): {_median(n_msgs)}")
        lines.append(f"- median tool calls/turn: {_median(tool_counts_per_turn)}")
        if tool_totals:
            by_name = ", ".join(f"{name}={count}" for name, count in sorted(tool_totals.items(), key=lambda kv: -kv[1]))
            lines.append(f"- tool calls by name (total across turns): {by_name}")
        else:
            lines.append("- tool calls by name (total across turns): (none)")
        for k in TOKEN_KEYS:
            vals = token_values[k]
            total = sum(vals) if vals else 0
            lines.append(f"- {TOKEN_LABELS[k]} tokens: total={total}, median/turn={_median(vals)}")
        lines.append("")
    return "\n".join(lines)


def _median(values):
    return statistics.median(values) if values else "n/a"


# ---------------------------------------------------------------------------
# 3. Per-cell numeric-error table
# ---------------------------------------------------------------------------


def _numeric_summary(task, computed):
    if not computed:
        return ""
    parts = []
    if task == "T01":
        for name, d in (computed.get("value_errors") or {}).items():
            if isinstance(d, dict) and d.get("best_err") is not None:
                parts.append(f"{name} err={d['best_err']:.3g}")
    elif task in ("T02", "T03"):
        d = computed.get("events_match") or {}
        if d:
            parts.append(f"matched={d.get('matched')}/{d.get('truth_total', '?')} spurious={len(d.get('spurious', []) or [])}")
    elif task == "T07":
        d = computed.get("fault_time") or {}
        if d.get("dt") is not None:
            parts.append(f"dt={d['dt']:.3g}s")
        elif d.get("best_dt") is not None:
            parts.append(f"best_dt={d['best_dt']:.3g}s (fail)")
    elif task == "T08":
        d = computed.get("marker_covers_fault") or {}
        if d.get("iou_proxy") is not None:
            parts.append(f"iou_proxy={d['iou_proxy']:.3g} covered_s={d.get('covered_s', '?')}")
    elif task == "T09":
        d = computed.get("interval_match") or {}
        if d.get("iou") is not None:
            parts.append(f"iou={d['iou']:.3g}")
    return "; ".join(parts)


def render_error_table(records):
    lines = ["## Per-cell numeric errors\n"]
    lines.append("| cell | task | status | pass | numeric detail |")
    lines.append("|---|---|---|---|---|")
    for r in records:
        if r.status == "not_run":
            lines.append(f"| {r.cell_id} | {r.task} | not run | | |")
            continue
        if r.score is None:
            lines.append(f"| {r.cell_id} | {r.task} | ran | not scored | |")
            continue
        detail = _numeric_summary(r.task, r.score.get("computed"))
        pass_mark = "✅" if r.score.get("pass") else "❌"
        lines.append(f"| {r.cell_id} | {r.task} | ran | {pass_mark} | {detail} |")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def render_report(run_dir):
    records = collect_turns(run_dir)
    if not records:
        return f"# Real-data benchmark report\n\nNo cells found under `{run_dir}`.\n"
    out = [f"# Real-data benchmark report: `{run_dir}`\n"]
    out.append(render_pass_matrix(records))
    out.append(render_model_stats(records))
    out.append(render_error_table(records))
    return "\n".join(out)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: realdata_report.py runs/<date>/", file=sys.stderr)
        return 2
    print(render_report(argv[0]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
