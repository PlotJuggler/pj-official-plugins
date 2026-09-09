#!/usr/bin/env python3
"""Paired comparison, flight by flight, of two benchmark runs of the same cells.

Compares a base run directory against a new run directory (and optionally a
control run) produced by this benchmark harness. Cells are directories named
``<dataset>-<file>-<tasks>-<model>-<arm>`` (tasks joined by ``+``); flights are
paired across runs by ``(file, tasks, model)``, ignoring the ``arm`` component
(the catalog size), since that is exactly what's expected to change between a
base and a new run.

For each paired flight we report, from ``<cell>/turn1/``:
  - pass/fail, taken from the most relevant ``score_<task>.json`` in the cell
    (preferring T07, then the --task filter if given, else the first task).
  - tool-use counts: describe_topic calls, single-path bucket reads of
    read_series, batched (multi-path) reads, and total tool calls.
  - wall clock seconds, output tokens and cache-read tokens.

It prints a Markdown report to stdout and also writes it to
``<new_run_dir>/COMPARE.md``. It is deliberately tolerant of missing files:
any missing metrics/score/stream file is treated as unavailable data (shown
as "-") with a short note, rather than a hard failure.

Usage:
    compare.py --base runs/2026-09-08 --new runs/2026-09-09-v2 \
        [--control runs/2026-09-09-v2/control] [--task T07] [--model sonnet]
"""

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

ANNOTATIONS_PATH = Path(__file__).resolve().parent / "truth" / "glance" / "annotations.json"

CELL_RE = re.compile(r"^([^-]+)-([^-]+)-([^-]+)-([^-]+)-([^-]+)$")

NUMERIC_COLUMNS = [
    ("describe_topic", "describe_topic"),
    ("single_buckets", "single_buckets"),
    ("batch_reads", "batch_reads"),
    ("total_calls", "total tool calls"),
    ("wall_s", "wall_s"),
    ("output_tokens", "output tokens"),
    ("cache_read_tokens", "cache_read tokens"),
]


def parse_cell_dirname(name):
    """Split a cell directory name into (dataset, file, tasks, model, arm)."""
    m = CELL_RE.match(name)
    if not m:
        return None
    dataset, file_, tasks, model, arm = m.groups()
    return dataset, file_, tasks, model, arm


def load_json(path, notes, label):
    if not path.exists():
        return None
    try:
        with path.open() as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        notes.append(f"{label}: could not read {path} ({e})")
        return None


def pick_primary_task(tasks_list, task_filter):
    if task_filter and task_filter in tasks_list:
        return task_filter
    if "T07" in tasks_list:
        return "T07"
    if tasks_list:
        return sorted(tasks_list)[0]
    return None


def load_score(cell_dir, tasks_list, task_filter, notes, label):
    """Return (pass_bool_or_None, primary_task_used)."""
    primary = pick_primary_task(tasks_list, task_filter)
    score = None
    used_task = None
    if primary is not None:
        p = cell_dir / f"score_{primary}.json"
        score = load_json(p, notes, label)
        if score is not None:
            used_task = primary
    if score is None:
        # fall back to the combined score.json (may reflect only the last
        # task run in a multi-task cell, but is better than nothing)
        p = cell_dir / "score.json"
        score = load_json(p, notes, label)
        if score is not None:
            used_task = score.get("task")
            notes.append(f"{label}: no score_{primary}.json, used score.json (task={used_task})")
    if score is None:
        return None, None
    return score.get("pass"), used_task


def count_read_series_calls(stream_path, notes, label):
    """Return (single_buckets, batch_reads) counted from stream.jsonl, or (None, None)."""
    if not stream_path.exists():
        return None, None
    single_buckets = 0
    batch_reads = 0
    try:
        with stream_path.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("type") != "assistant":
                    continue
                content = obj.get("message", {}).get("content", [])
                if not isinstance(content, list):
                    continue
                for block in content:
                    if not isinstance(block, dict):
                        continue
                    if block.get("type") != "tool_use":
                        continue
                    if block.get("name") != "mcp__pj__read_series":
                        continue
                    inp = block.get("input", {})
                    if not isinstance(inp, dict):
                        continue
                    if inp.get("mode") != "buckets":
                        continue
                    paths = inp.get("paths")
                    if isinstance(paths, str):
                        single_buckets += 1
                    elif isinstance(paths, list):
                        if len(paths) <= 1:
                            single_buckets += 1
                        else:
                            batch_reads += 1
    except OSError as e:
        notes.append(f"{label}: could not read {stream_path} ({e})")
        return None, None
    return single_buckets, batch_reads


def load_cell(cell_dir, tasks_list, task_filter, notes, label):
    """Load all metrics for one cell's turn1. Returns a dict of metric -> value (None if unavailable)."""
    turn1 = cell_dir / "turn1"
    metrics = load_json(turn1 / "metrics.json", notes, label)

    wall_s = None
    tool_use_counts = {}
    output_tokens = None
    cache_read_tokens = None
    total_calls = None
    describe_topic = None

    if metrics is None:
        notes.append(f"{label}: missing turn1/metrics.json in {cell_dir}")
    else:
        wall_s = metrics.get("wall_s")
        tool_use_counts = metrics.get("tool_use_counts", {}) or {}
        total_calls = sum(tool_use_counts.values()) if tool_use_counts else 0
        describe_topic = tool_use_counts.get("mcp__pj__describe_topic", 0)
        usage = (metrics.get("result") or {}).get("usage", {}) or {}
        output_tokens = usage.get("output_tokens")
        cache_read_tokens = usage.get("cache_read_input_tokens")

    single_buckets, batch_reads = count_read_series_calls(turn1 / "stream.jsonl", notes, label)
    if single_buckets is None:
        notes.append(f"{label}: missing/unreadable turn1/stream.jsonl in {cell_dir}")

    pass_val, used_task = load_score(cell_dir, tasks_list, task_filter, notes, label)
    if pass_val is None:
        notes.append(f"{label}: no usable score_*.json in {cell_dir}")

    return {
        "pass": pass_val,
        "used_task": used_task,
        "describe_topic": describe_topic,
        "single_buckets": single_buckets,
        "batch_reads": batch_reads,
        "total_calls": total_calls,
        "wall_s": wall_s,
        "output_tokens": output_tokens,
        "cache_read_tokens": cache_read_tokens,
    }


def collect_run(run_dir, task_filter, model_filter, notes, label):
    """Scan a run directory for cells, return {(file, tasks, model): cell_info}."""
    cells = {}
    if not run_dir.exists():
        notes.append(f"{label}: run directory {run_dir} does not exist")
        return cells
    for entry in sorted(run_dir.iterdir()):
        if not entry.is_dir():
            continue
        parsed = parse_cell_dirname(entry.name)
        if parsed is None:
            continue
        dataset, file_, tasks, model, arm = parsed
        tasks_list = tasks.split("+")
        if task_filter and task_filter not in tasks_list:
            continue
        if model_filter and model != model_filter:
            continue
        # With a task filter, pair on the task itself so a T07-only cell matches a
        # T07+T08 cell from another run (turn 1 is identical in both).
        key = (file_, task_filter if task_filter else tasks, model)
        if key in cells:
            notes.append(
                f"{label}: duplicate cell for {key} (arms {cells[key]['arm']!r} and {arm!r}); "
                f"keeping {cells[key]['arm']!r}"
            )
            continue
        info = load_cell(entry, tasks_list, task_filter, notes, label)
        info["dataset"] = dataset
        info["tasks"] = tasks
        info["model"] = model
        info["arm"] = arm
        info["dir"] = entry
        cells[key] = info
    return cells


def flight_id(key):
    return key[0]


def fmt_pass(v):
    if v is None:
        return "—"
    return "✅" if v else "❌"


def fmt_num(v, digits=0):
    if v is None:
        return "—"
    if digits:
        return f"{v:.{digits}f}"
    return f"{v}"


def median_or_none(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return statistics.median(vals)


def load_annotations(notes):
    ann = load_json(ANNOTATIONS_PATH, notes, "annotations")
    if ann is None:
        notes.append(f"annotations: could not load {ANNOTATIONS_PATH}, glance column will be blank")
        return {}
    return ann


def glance_for(annotations, file_):
    entry = annotations.get(file_)
    if not isinstance(entry, dict):
        return "?"
    return entry.get("glance", "?")


def build_table(base_cells, new_cells, annotations):
    keys = sorted(set(base_cells) | set(new_cells), key=flight_id)
    header = [
        "file", "glance",
        "base pass", "base describe", "base single", "base batch", "base total", "base wall_s", "base out_tok", "base cache_read",
        "new pass", "new describe", "new single", "new batch", "new total", "new wall_s", "new out_tok", "new cache_read",
    ]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    rows = []
    for key in keys:
        b = base_cells.get(key)
        n = new_cells.get(key)
        file_ = flight_id(key)
        glance = glance_for(annotations, file_)
        row = [file_, glance]
        for cell in (b, n):
            if cell is None:
                row += ["—"] * 8
            else:
                row += [
                    fmt_pass(cell["pass"]),
                    fmt_num(cell["describe_topic"]),
                    fmt_num(cell["single_buckets"]),
                    fmt_num(cell["batch_reads"]),
                    fmt_num(cell["total_calls"]),
                    fmt_num(cell["wall_s"], 1),
                    fmt_num(cell["output_tokens"]),
                    fmt_num(cell["cache_read_tokens"]),
                ]
        lines.append("| " + " | ".join(row) + " |")
        rows.append((key, b, n))
    return "\n".join(lines), rows


def medians_row(cells_by_key, label):
    values = {col: [] for col, _ in NUMERIC_COLUMNS}
    passed = 0
    known = 0
    for info in cells_by_key.values():
        for col, _ in NUMERIC_COLUMNS:
            values[col].append(info.get(col))
        if info.get("pass") is not None:
            known += 1
            if info["pass"]:
                passed += 1
    medians = {col: median_or_none(values[col]) for col, _ in NUMERIC_COLUMNS}
    return label, medians, passed, known


def build_medians_table(rows_by_label):
    header = ["run"] + [name for _, name in NUMERIC_COLUMNS] + ["pass (k/n)"]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for label, medians, passed, known in rows_by_label:
        row = [label]
        for col, _ in NUMERIC_COLUMNS:
            v = medians[col]
            digits = 1 if col == "wall_s" else 0
            row.append(fmt_num(v, digits))
        row.append(f"{passed}/{known}")
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def evaluate_rule(new_medians, base_cells, new_cells, annotations):
    reasons = []
    describe_med = new_medians["describe_topic"]
    single_med = new_medians["single_buckets"]
    total_med = new_medians["total_calls"]

    if describe_med is None or describe_med > 3:
        reasons.append(f"describe_topic median = {fmt_num(describe_med)} (want <= 3)")
    if single_med is None or single_med > 3:
        reasons.append(f"single_buckets median = {fmt_num(single_med)} (want <= 3)")
    if total_med is None or total_med > 18:
        reasons.append(f"total tool calls median = {fmt_num(total_med)} (want <= 18)")

    lost = []
    all_keys = set(base_cells) | set(new_cells)
    for key in sorted(all_keys, key=flight_id):
        file_ = flight_id(key)
        entry = annotations.get(file_)
        if not isinstance(entry, dict) or entry.get("glance") != "obvious":
            continue
        b = base_cells.get(key)
        n = new_cells.get(key)
        if b is None or n is None:
            continue
        if b.get("pass") is True and n.get("pass") is False:
            lost.append(file_)

    if lost:
        reasons.append("obvious flights that regressed from pass to fail: " + ", ".join(lost))

    return (len(reasons) == 0), reasons, lost


def build_diffs(base_cells, new_cells):
    keys = sorted(set(base_cells) & set(new_cells), key=flight_id)
    header = ["file", "Δ total calls (new-base)", "Δ wall_s (new-base)"]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    total_diffs = []
    wall_diffs = []
    for key in keys:
        b = base_cells[key]
        n = new_cells[key]
        row = [flight_id(key)]
        if b["total_calls"] is not None and n["total_calls"] is not None:
            d = n["total_calls"] - b["total_calls"]
            total_diffs.append(d)
            row.append(fmt_num(d))
        else:
            row.append("—")
        if b["wall_s"] is not None and n["wall_s"] is not None:
            d = n["wall_s"] - b["wall_s"]
            wall_diffs.append(d)
            row.append(fmt_num(d, 1))
        else:
            row.append("—")
        lines.append("| " + " | ".join(row) + " |")
    table = "\n".join(lines)

    med_total = median_or_none(total_diffs)
    med_wall = median_or_none(wall_diffs)
    improved_total = sum(1 for d in total_diffs if d < 0)
    improved_wall = sum(1 for d in wall_diffs if d < 0)

    summary = (
        f"Median diff (total calls): {fmt_num(med_total)}; improved (fewer calls) in "
        f"{improved_total}/{len(total_diffs)} flights.\n"
        f"Median diff (wall_s): {fmt_num(med_wall, 1)}; improved (faster) in "
        f"{improved_wall}/{len(wall_diffs)} flights."
    )
    return table, summary


def build_control_section(base_cells, control_cells):
    keys = sorted(set(base_cells) & set(control_cells), key=flight_id)
    if not keys:
        return None
    header = [
        "file",
        "base total", "base wall_s",
        "control total", "control wall_s",
        "Δ total (control-base)", "Δ wall_s (control-base)",
    ]
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for key in keys:
        b = base_cells[key]
        c = control_cells[key]
        row = [flight_id(key), fmt_num(b["total_calls"]), fmt_num(b["wall_s"], 1),
               fmt_num(c["total_calls"]), fmt_num(c["wall_s"], 1)]
        if b["total_calls"] is not None and c["total_calls"] is not None:
            row.append(fmt_num(c["total_calls"] - b["total_calls"]))
        else:
            row.append("—")
        if b["wall_s"] is not None and c["wall_s"] is not None:
            row.append(fmt_num(c["wall_s"] - b["wall_s"], 1))
        else:
            row.append("—")
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="base run directory, e.g. runs/2026-09-08")
    ap.add_argument("--new", required=True, help="new run directory, e.g. runs/2026-09-09-v2")
    ap.add_argument("--control", default=None, help="optional control run directory, e.g. runs/2026-09-09-v2/control")
    ap.add_argument("--task", default=None, help="optional task filter, e.g. T07")
    ap.add_argument("--model", default=None, help="optional model filter, e.g. sonnet")
    args = ap.parse_args()

    base_dir = Path(args.base)
    new_dir = Path(args.new)
    control_dir = Path(args.control) if args.control else None

    notes = []
    annotations = load_annotations(notes)

    base_cells = collect_run(base_dir, args.task, args.model, notes, "base")
    new_cells = collect_run(new_dir, args.task, args.model, notes, "new")
    control_cells = collect_run(control_dir, args.task, args.model, notes, "control") if control_dir else {}

    out = []
    out.append(f"# Compare: {base_dir} vs {new_dir}")
    if args.task or args.model:
        out.append(f"Filters: task={args.task or 'any'} model={args.model or 'any'}")
    out.append("")

    out.append("## 1. Per-flight comparison")
    out.append("")
    table, rows = build_table(base_cells, new_cells, annotations)
    out.append(table)
    out.append("")

    out.append("## 2. Medians per run")
    out.append("")
    medians_rows = [medians_row(base_cells, "base"), medians_row(new_cells, "new")]
    if control_dir:
        medians_rows.append(medians_row(control_cells, "control"))
    out.append(build_medians_table(medians_rows))
    out.append("")

    out.append("## 3. Decision rule")
    out.append("")
    out.append(
        "Rule: median(new) describe_topic <= 3, median(new) single_buckets <= 3, "
        "median(new) total tool calls <= 18, and no flight tagged 'obvious' that passed "
        "in base regresses to fail in new."
    )
    new_medians = medians_rows[1][1]
    ok, reasons, lost = evaluate_rule(new_medians, base_cells, new_cells, annotations)
    if ok:
        out.append("")
        out.append("RULE: PASS")
    else:
        out.append("")
        out.append("RULE: FAIL (" + "; ".join(reasons) + ")")
    out.append("")

    out.append("## 4. Paired differences (new - base)")
    out.append("")
    diffs_table, diffs_summary = build_diffs(base_cells, new_cells)
    out.append(diffs_table)
    out.append("")
    out.append(diffs_summary)
    out.append("")

    if control_dir:
        out.append("## 5. Control (day-to-day drift with the old plugin)")
        out.append("")
        control_table = build_control_section(base_cells, control_cells)
        if control_table is None:
            out.append("_No overlapping flights between base and control._")
        else:
            out.append(control_table)
        out.append("")

    if notes:
        out.append("## Notes")
        out.append("")
        for n in notes:
            out.append(f"- {n}")
        out.append("")

    report = "\n".join(out)
    print(report)

    compare_path = new_dir / "COMPARE.md"
    try:
        compare_path.write_text(report + "\n")
    except OSError as e:
        print(f"\n(warning: could not write {compare_path}: {e})", file=sys.stderr)


if __name__ == "__main__":
    main()
