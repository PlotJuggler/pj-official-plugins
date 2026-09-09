#!/usr/bin/env python3
"""Scoring for the real-data Assistant Agent benchmark (the SCORING side).

Reads what `bench_cli.py` left in `run_dir/<cell>/turn<N>/` (see README.md's
"What lands on disk, per turn" table), compares the model's final ```json
block against a ground-truth file under `truth/`, and writes `score.json`
next to the cell.

Python 3 stdlib only.

------------------------------------------------------------------------
Marker coverage (T08) -- documented limitation
------------------------------------------------------------------------
`create_markers`'s tool result (src/tools.cpp:62-119, `publishedMarkerSummary`)
reports `markers_created`, `by_kind` (counts) and `covered_s` (the UNION
duration of region markers) -- it never reports individual region start/end
timestamps. That means true IoU against the truth interval cannot be computed
from this schema: there is no position information to intersect. This module
approximates it with `iou_proxy = min(covered_s, L) / max(covered_s, L)`
(`L` = truth interval length), which equals the true IoU only if the marked
region(s) are optimally aligned inside/around the truth interval -- an
optimistic upper bound, not a verified overlap. `marker_covers_fault`'s
`detail` always carries this caveat so a report can surface it. See the
README's "Scoring" section.
"""
import argparse
import json
import re
import sys
from pathlib import Path

TOOL_PREFIX = "mcp__pj__"
DEFAULT_TOLERANCE_S = 2.0


# ---------------------------------------------------------------------------
# Axis conversion (truth's absolute clock -> PlotJuggler's display-axis
# seconds). ONE function, used everywhere truth times are compared against a
# model's `t_s`.
# ---------------------------------------------------------------------------


def truth_to_display_s(t_abs, time_axis, offset_s=0.0):
    """Convert an absolute truth timestamp to PlotJuggler's display axis.

    scale is 1 for second-based axes (`ros_epoch`, `csv_datetime_utc`) and
    1e-6 for `ulog_us_since_boot` (PX4 ULog microseconds-since-boot). `offset_s`
    comes from truth/axis_calibration.json (default 0.0).
    """
    kind = time_axis.get("kind", "") if isinstance(time_axis, dict) else ""
    unit = time_axis.get("unit", "") if isinstance(time_axis, dict) else ""
    # compute_truth.py already stores ULog times in seconds (unit "s"); the
    # microsecond scale only applies when a truth file says so explicitly.
    scale = 1e-6 if (kind == "ulog_us_since_boot" and unit == "us") else 1.0
    first = time_axis.get("first_sample_abs", 0) if isinstance(time_axis, dict) else 0
    return (t_abs - first) * scale + offset_s


# ---------------------------------------------------------------------------
# truth / calibration loading
# ---------------------------------------------------------------------------


def load_truth(truth_dir, task, file_id):
    truth_dir = Path(truth_dir)
    candidates = []
    if file_id:
        candidates.append(truth_dir / f"{task}_{file_id}.json")
    candidates.append(truth_dir / f"{task}.json")
    for c in candidates:
        if c.is_file():
            return json.loads(c.read_text(encoding="utf-8"))
    tried = ", ".join(str(c) for c in candidates)
    raise FileNotFoundError(f"no truth file for task={task!r} file={file_id!r} (tried: {tried})")


def load_calibration_offset(truth_dir, file_id):
    path = Path(truth_dir) / "axis_calibration.json"
    if not file_id or not path.is_file():
        return 0.0
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return 0.0
    entry = doc.get(file_id) if isinstance(doc, dict) else None
    if not isinstance(entry, dict):
        return 0.0
    try:
        return float(entry.get("offset_s", 0.0))
    except (TypeError, ValueError):
        return 0.0


# ---------------------------------------------------------------------------
# Cell directory naming: "<dataset>-<file-stem>-<turns joined with '+'>-
# <model>-<arm>" (README.md, "Where output lands"). `cell.json`, when
# present inside the cell dir, overrides any field it names -- it is the
# authoritative source if the driver ever writes one; the name is parsed only
# as a fallback / cross-check.
# ---------------------------------------------------------------------------

_CELL_DIR_RE = re.compile(
    r"^(?P<dataset>px4|alfa|skab)-(?P<file>.+)-(?P<tasks>T\d\d(?:\+T\d\d)*)-(?P<model>[^-]+)-(?P<arm>catalog6000|catalogfull)$"
)


def parse_cell_dir(cell_dir):
    cell_dir = Path(cell_dir)
    info = {}
    cell_json = cell_dir / "cell.json"
    if cell_json.is_file():
        try:
            info = json.loads(cell_json.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            info = {}
    m = _CELL_DIR_RE.match(cell_dir.name)

    def pick(key, group=None):
        if isinstance(info.get(key), str) and info.get(key):
            return info[key]
        return m.group(group) if (m and group) else None

    dataset = pick("dataset", "dataset")
    file_id = pick("file", "file")
    model = pick("model", "model")
    arm = pick("arm", "arm")

    turns = None
    if isinstance(info.get("turns"), list) and info["turns"]:
        turns = [str(t) for t in info["turns"]]
    elif isinstance(info.get("task"), str) and info["task"]:
        turns = [info["task"]]
    elif m:
        turns = m.group("tasks").split("+")
    return {"dataset": dataset, "file": file_id, "model": model, "arm": arm, "turns": turns or []}


# ---------------------------------------------------------------------------
# stream.jsonl: reply text, the final ```json block, and tool_use/tool_result
# pairing.
# ---------------------------------------------------------------------------


def read_stream_records(stream_path):
    records = []
    try:
        text = Path(stream_path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return records
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


def assistant_reply_text(records):
    """Concatenation of the text blocks of every `assistant` message, in
    stream order (task brief: "The model's reply text = concatenation of the
    text blocks of assistant messages in stream.jsonl")."""
    parts = []
    for rec in records:
        if not isinstance(rec, dict) or rec.get("type") != "assistant":
            continue
        content = rec.get("message", {})
        content = content.get("content", []) if isinstance(content, dict) else []
        if not isinstance(content, list):
            continue
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                t = block.get("text", "")
                if t:
                    parts.append(t)
    return "\n".join(parts)


_FENCE_RE = re.compile(r"```json\s*(.*?)```", re.DOTALL | re.IGNORECASE)


def _find_brace_spans(text):
    """Top-level {...} spans, ignoring braces inside JSON string literals."""
    spans = []
    depth = 0
    start = None
    in_string = False
    escape = False
    for i, ch in enumerate(text):
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
                if depth == 0 and start is not None:
                    spans.append((start, i + 1))
                    start = None
    return spans


def extract_json_block(reply_text):
    """The LAST ```json fenced block that parses as a JSON object; if none,
    the last bare `{...}` that parses as an object. Returns (obj, how) or
    (None, None)."""
    fenced = _FENCE_RE.findall(reply_text)
    for candidate in reversed(fenced):
        try:
            obj = json.loads(candidate.strip())
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            return obj, "fenced"
    for start, end in reversed(_find_brace_spans(reply_text)):
        try:
            obj = json.loads(reply_text[start:end])
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            return obj, "bare"
    return None, None


def _tool_result_text(block):
    content = block.get("content")
    if isinstance(content, list):
        for c in content:
            if isinstance(c, dict) and c.get("type") == "text":
                return c.get("text")
    if isinstance(content, str):
        return content
    return None


def get_tool_results(records, tool_names):
    """Tool results for the given (unprefixed) tool names, in stream order.
    Each entry: {"name", "text", "parsed"} -- `parsed` is None when the text
    isn't JSON."""
    id_to_name = {}
    results = []
    for rec in records:
        if not isinstance(rec, dict):
            continue
        rtype = rec.get("type")
        message = rec.get("message", {})
        content = message.get("content", []) if isinstance(message, dict) else []
        if not isinstance(content, list):
            continue
        if rtype == "assistant":
            for b in content:
                if isinstance(b, dict) and b.get("type") == "tool_use":
                    name = b.get("name", "")
                    if name.startswith(TOOL_PREFIX):
                        name = name[len(TOOL_PREFIX):]
                    id_to_name[b.get("id")] = name
        elif rtype == "user":
            for b in content:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    name = id_to_name.get(b.get("tool_use_id"))
                    if name in tool_names:
                        text = _tool_result_text(b)
                        parsed = None
                        if isinstance(text, str):
                            try:
                                parsed = json.loads(text)
                            except json.JSONDecodeError:
                                parsed = None
                        results.append({"name": name, "text": text, "parsed": parsed})
    return results


def catalog_prefix(stdin_sent_text):
    """The catalog part of `stdin_sent.txt`: everything before the LAST
    "\\n\\n" (mirrors bench_cli.py's `split_payload`); "" if there is none."""
    if "\n\n" in stdin_sent_text:
        prefix, _, _ = stdin_sent_text.rpartition("\n\n")
        return prefix + "\n\n"
    return ""


# ---------------------------------------------------------------------------
# Catalog parsing, for `no_invented_sources`. Mirrors the digest built by
# `catalogDigest` (src/tools.cpp): topic lines are exactly "  <topic>[: field
# (type), ...]"; dataset headers are unindented `dataset "<name>":` lines.
# ---------------------------------------------------------------------------


def parse_catalog_topics(catalog_text):
    topics = []
    for raw_line in catalog_text.splitlines():
        if not raw_line.startswith("  "):
            continue
        line = raw_line[2:]
        topic = line.split(":", 1)[0].strip()
        if topic:
            topics.append(topic)
    truncated = "TRUNCATED" in catalog_text
    names_only = "Field names are omitted" in catalog_text
    return topics, truncated, names_only


def _topic_prefixes(topic):
    segs = [s for s in topic.split("/") if s]
    prefixes = set()
    cur = ""
    for s in segs:
        cur += "/" + s
        prefixes.add(cur)
    return prefixes


def _source_ok(src, all_prefixes, relaxed):
    s = src.strip()
    if not s:
        return False
    if not s.startswith("/"):
        s = "/" + s
    segs = [x for x in s.split("/") if x]
    if not segs:
        return False
    if relaxed:
        # Catalog may be truncated / names-only: an unseen topic could still
        # be real. Only require the first path segment to be a real branch.
        return ("/" + segs[0]) in all_prefixes
    for k in range(len(segs), 0, -1):
        candidate = "/" + "/".join(segs[:k])
        if candidate in all_prefixes:
            return True
    return False


def _collect_sources(block):
    sources = []
    for e in block.get("events", []) or []:
        if isinstance(e, dict) and e.get("source"):
            sources.append(str(e["source"]))
    for v in (block.get("values") or {}).values():
        if isinstance(v, dict) and v.get("source"):
            sources.append(str(v["source"]))
    return sources


def _created_names_from_turn(turn_dir):
    """Series the model created in this turn (create_derived_series tool_use
    names in stream.jsonl, plus list_created from the audit): a source that
    cites one of them is not invented."""
    names = set()
    turn_dir = Path(turn_dir)
    stream_path = turn_dir / "stream.jsonl"
    if stream_path.is_file():
        for line in stream_path.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("type") != "assistant":
                continue
            for blk in (rec.get("message") or {}).get("content", []) or []:
                if blk.get("type") == "tool_use" and str(blk.get("name", "")).endswith("create_derived_series"):
                    name = (blk.get("input") or {}).get("name")
                    if name:
                        names.add(str(name))
    outcome_path = turn_dir / "outcome.json"
    if outcome_path.is_file():
        try:
            names |= set(_parse_list_created(json.loads(outcome_path.read_text(encoding="utf-8"))))
        except (ValueError, OSError):
            pass
    return names


def _cites_created(src, created_names):
    s = str(src).strip()
    if s.lower().startswith("derived") or " - " in s or " + " in s or " vs " in s or " minus " in s:
        return True  # an expression, not a topic claim
    if any(w in s.lower() for w in ("playback", "report_status", "evaluate")):
        return True  # a tool's own output (e.g. the playback range), not a topic claim
    head = s.lstrip("/").split("/")[0]
    return s in created_names or head in created_names or any(s.startswith(n) for n in created_names)


def check_no_invented_sources(block, catalog_text, created_names=()):
    if not isinstance(block, dict):
        return "", {}
    topics, truncated, names_only = parse_catalog_topics(catalog_text)
    if not topics:
        return "", {"skipped": "no catalog topics parsed from stdin_sent.txt"}
    all_prefixes = set()
    for t in topics:
        all_prefixes |= _topic_prefixes(t)
    relaxed = truncated or names_only
    created_names = set(created_names)
    bad = [s for s in _collect_sources(block)
           if not _source_ok(s, all_prefixes, relaxed) and not _cites_created(s, created_names)]
    if bad:
        return f"sources not found in catalog: {bad}", {"bad_sources": bad, "relaxed": relaxed}
    return "", {"relaxed": relaxed}


# ---------------------------------------------------------------------------
# kinds_valid (all tasks with a `kinds` entry in tasks.json)
# ---------------------------------------------------------------------------


def check_kinds_valid(block, task_id, kinds_table):
    allowed = kinds_table.get(task_id)
    if not allowed:
        return "", {}
    # A "<kind>_end" pairing is tolerated for interval-shaped answers (e.g.
    # T09's anomaly/anomaly_end) even though tasks.json only lists the start
    # kind -- see README "Scoring" for this assumption.
    allowed_set = set(allowed) | {k + "_end" for k in allowed}
    events = block.get("events", []) if isinstance(block, dict) else []
    bad = sorted({e.get("kind") for e in events if isinstance(e, dict) and e.get("kind") not in allowed_set})
    if bad:
        return f"kind(s) not in {sorted(allowed)} (or '*_end'): {bad}", {"bad_kinds": bad}
    return "", {}


# ---------------------------------------------------------------------------
# T01: values_within_tolerance, source_named
# ---------------------------------------------------------------------------


def check_values_within_tolerance(block, truth, time_axis, offset_s, value_names):
    model_values = block.get("values", {}) if isinstance(block, dict) else {}
    if not isinstance(model_values, dict):
        model_values = {}
    truth_values = truth.get("values", {}) if isinstance(truth, dict) else {}
    bad = []
    detail = {}
    for name in value_names:
        tv = truth_values.get(name)
        if not isinstance(tv, dict):
            continue
        mv = model_values.get(name)
        if not isinstance(mv, dict) or "value" not in mv:
            bad.append(f"{name}: missing from model output")
            detail[name] = {"model": None}
            continue
        model_val = mv["value"]
        tol = tv.get("tolerance", 0.0)
        candidates = tv.get("candidates", [])
        is_time = name.endswith("_s")
        ok = False
        best_err = None
        for cand in candidates:
            if not isinstance(cand, dict) or "value" not in cand:
                continue
            truth_cmp = truth_to_display_s(cand["value"], time_axis, offset_s) if is_time else cand["value"]
            try:
                err = abs(float(model_val) - float(truth_cmp))
            except (TypeError, ValueError):
                continue
            if best_err is None or err < best_err:
                best_err = err
            if err <= tol:
                ok = True
        detail[name] = {"model": model_val, "best_err": best_err, "tolerance": tol}
        if not ok:
            bad.append(f"{name}: {model_val} not within {tol} of any candidate (best err {best_err})")
    if bad:
        return "; ".join(bad), detail
    return "", detail


def check_source_named(block, value_names):
    model_values = block.get("values", {}) if isinstance(block, dict) else {}
    if not isinstance(model_values, dict):
        model_values = {}
    missing = [
        n
        for n in value_names
        if not (isinstance(model_values.get(n), dict) and str(model_values[n].get("source") or "").strip())
    ]
    if missing:
        return f"missing/empty source for: {missing}", {"missing": missing}
    return "", {}


def check_assumptions_present(block):
    assumptions = block.get("assumptions") if isinstance(block, dict) else None
    if isinstance(assumptions, list) and len(assumptions) > 0:
        return ""
    return "assumptions list is empty"


# ---------------------------------------------------------------------------
# Greedy nearest-time event matching, shared by T02/T03/T07.
# ---------------------------------------------------------------------------


def match_events_greedy(model_events, truth_events, time_axis, offset_s):
    truth_display = []
    for te in truth_events:
        t_disp = truth_to_display_s(te["t_abs"], time_axis, offset_s)
        truth_display.append({"t": t_disp, "tol": te.get("tolerance_s", DEFAULT_TOLERANCE_S), "kind": te.get("kind")})
    pairs = []
    for mi, me in enumerate(model_events):
        if not isinstance(me, dict) or not isinstance(me.get("t_s"), (int, float)):
            continue
        for ti, td in enumerate(truth_display):
            dist = abs(me["t_s"] - td["t"])
            if dist <= td["tol"]:
                pairs.append((dist, mi, ti))
    pairs.sort(key=lambda p: p[0])
    used_m, used_t = set(), set()
    matches = []
    for dist, mi, ti in pairs:
        if mi in used_m or ti in used_t:
            continue
        used_m.add(mi)
        used_t.add(ti)
        matches.append({"model_index": mi, "truth_index": ti, "dt": dist})
    missed = [i for i in range(len(truth_display)) if i not in used_t]
    spurious = [i for i in range(len(model_events)) if i not in used_m]
    return matches, missed, spurious


# ---------------------------------------------------------------------------
# T02: events_match (>= 80% of truth matched, <= 1 spurious)
# ---------------------------------------------------------------------------


def check_events_match_t02(block, truth, time_axis, offset_s):
    model_events = block.get("events", []) if isinstance(block, dict) else []
    truth_events = truth.get("events", []) if isinstance(truth, dict) else []
    matches, missed, spurious = match_events_greedy(model_events, truth_events, time_axis, offset_s)
    n_truth = len(truth_events)
    matched_ratio = (len(matches) / n_truth) if n_truth else 1.0
    ok = matched_ratio >= 0.8 and len(spurious) <= 1
    detail = {
        "matched": len(matches),
        "truth_total": n_truth,
        "missed": missed,
        "spurious": spurious,
        "matched_ratio": matched_ratio,
        "matches": matches,
    }
    reason = "" if ok else f"matched {len(matches)}/{n_truth} (need >=80%), spurious={len(spurious)} (need <=1)"
    return reason, detail


# ---------------------------------------------------------------------------
# T03 (open): events_match with the task's "none" escape hatch
# ---------------------------------------------------------------------------


def check_events_match_t03(block, truth, time_axis, offset_s):
    model_events = block.get("events", []) if isinstance(block, dict) else []
    truth_events = truth.get("events", []) if isinstance(truth, dict) else []
    if truth_events:
        for me in model_events:
            if not isinstance(me, dict) or me.get("kind") != "actuator_saturation":
                continue
            t = me.get("t_s")
            if not isinstance(t, (int, float)):
                continue
            for te in truth_events:
                tol = te.get("tolerance_s", DEFAULT_TOLERANCE_S)
                t_start = truth_to_display_s(te["t_abs"], time_axis, offset_s)
                t_end = truth_to_display_s(te.get("t_end_abs", te["t_abs"]), time_axis, offset_s)
                if abs(t - t_start) <= tol or (t_start - tol) <= t <= (t_end + tol):
                    return "", {"matched_model_t_s": t, "truth_interval": [t_start, t_end]}
        return "no model actuator_saturation event within tolerance of any truth interval", {"truth_count": len(truth_events)}
    kinds = [e.get("kind") for e in model_events if isinstance(e, dict)]
    if not model_events or all(k == "none" for k in kinds):
        return "", {"truth": "none"}
    return "truth has no saturation intervals but model reported event(s)", {"model_kinds": kinds}


# ---------------------------------------------------------------------------
# T07: fault_time, fault_kind, fault_surface (optional)
# ---------------------------------------------------------------------------


def _truth_fault_event(truth):
    events = truth.get("events", []) if isinstance(truth, dict) else []
    return events[0] if events else None


def check_fault_time(block, truth, time_axis, offset_s):
    model_events = block.get("events", []) if isinstance(block, dict) else []
    te = _truth_fault_event(truth)
    if te is None or te.get("kind") == "no_fault":
        model_kinds = [e.get("kind") for e in model_events if isinstance(e, dict)]
        if not model_events or all(k == "no_fault" for k in model_kinds):
            return "", {"truth": "no_fault"}
        return "truth says no_fault but model reported fault event(s)", {"model_kinds": model_kinds}
    t_truth = truth_to_display_s(te["t_abs"], time_axis, offset_s)
    tol = te.get("tolerance_s", DEFAULT_TOLERANCE_S)
    best = None
    for me in model_events:
        if not isinstance(me, dict) or me.get("kind") in (None, "no_fault"):
            continue
        t = me.get("t_s")
        if not isinstance(t, (int, float)):
            continue
        dt = abs(t - t_truth)
        if best is None or dt < best[0]:
            best = (dt, me)
    if best is not None and best[0] <= tol:
        return "", {"dt": best[0], "matched_event": best[1], "truth_t_s": t_truth, "tolerance_s": tol}
    return (
        f"no non-no_fault model event within {tol}s of truth t={t_truth:.2f}",
        {"best_dt": best[0] if best else None, "truth_t_s": t_truth, "tolerance_s": tol},
    )


def check_fault_kind(block, truth, time_axis, offset_s):
    reason, detail = check_fault_time(block, truth, time_axis, offset_s)
    te = _truth_fault_event(truth)
    if te is None or te.get("kind") == "no_fault":
        return "", {}
    if reason:
        return "fault_time failed, cannot compare kind", {}
    matched = detail.get("matched_event") or {}
    if matched.get("kind") == te.get("kind"):
        return "", {"model_kind": matched.get("kind"), "truth_kind": te.get("kind")}
    return (
        f"kind mismatch: model={matched.get('kind')!r} truth={te.get('kind')!r}",
        {"model_kind": matched.get("kind"), "truth_kind": te.get("kind")},
    )


def check_fault_surface(block, truth, matched_event):
    """`matched_event` is `check_fault_time`'s own `detail["matched_event"]` --
    passed in rather than re-derived, so this never re-runs the time match
    with a different (or missing) calibration offset than the rest of the
    scoring pass used."""
    te = _truth_fault_event(truth)
    if te is None:
        return ""
    truth_detail = (te.get("detail") or "").strip().lower()
    if not truth_detail:
        return ""
    matched = matched_event
    if not matched:
        return "no matched model event to compare surface detail against"
    model_detail = (matched.get("detail") or "").strip().lower()
    surfaces = ("aileron", "rudder", "elevator", "engine")
    truth_words = [w for w in surfaces if w in truth_detail]
    if truth_words and all(w in model_detail for w in truth_words):
        return ""
    if truth_detail in model_detail or model_detail in truth_detail:
        return ""
    return f"surface detail mismatch: model={model_detail!r} truth={truth_detail!r}"


# ---------------------------------------------------------------------------
# T08 (turn 2 of the T07->T08 cell): marker_covers_fault, series_created,
# tab_created.
# ---------------------------------------------------------------------------


def _parse_list_created(outcome):
    if isinstance(outcome.get("created_ids"), list):
        return [c for c in outcome["created_ids"] if isinstance(c, str)]
    lc = outcome.get("list_created")
    if isinstance(lc, dict) and lc.get("ok"):
        try:
            parsed = json.loads(lc.get("text", "") or "")
        except json.JSONDecodeError:
            parsed = None
        if isinstance(parsed, dict) and isinstance(parsed.get("created"), list):
            return [c for c in parsed["created"] if isinstance(c, str)]
    return []


def _interval_length(truth, time_axis, offset_s):
    te = _truth_fault_event(truth)
    if te is None or te.get("kind") == "no_fault":
        return None, None
    t_start = truth_to_display_s(te["t_abs"], time_axis, offset_s)
    # compute_truth.py stores the bag end under values.post_fault_end_abs.value;
    # a top-level key is accepted too.
    end_abs = truth.get("post_fault_end_abs")
    if end_abs is None:
        nested = (truth.get("values") or {}).get("post_fault_end_abs")
        if isinstance(nested, dict):
            end_abs = nested.get("value")
    if end_abs is None:
        end_abs = te.get("t_end_abs", te["t_abs"])
    t_end = truth_to_display_s(end_abs, time_axis, offset_s)
    return t_start, t_end


def check_marker_covers_fault(records, truth, time_axis, offset_s):
    t_start, t_end = _interval_length(truth, time_axis, offset_s)
    if t_start is None:
        return "", {"skipped": "no truth fault interval to cover"}
    length_s = max(t_end - t_start, 1e-9)
    marker_results = get_tool_results(records, {"create_markers"})
    if not marker_results:
        return "model never called create_markers", {"truth_interval_s": length_s}
    last = marker_results[-1]["parsed"] or {}
    covered_s = last.get("covered_s")
    by_kind = last.get("by_kind", {})
    if not isinstance(covered_s, (int, float)) or covered_s <= 0:
        return (
            "create_markers reported no region coverage (covered_s missing/zero -- "
            "check by_kind: only region markers have a duration)",
            {"truth_interval_s": length_s, "by_kind": by_kind},
        )
    # See the module docstring: covered_s is a UNION duration, not positions.
    iou_proxy = min(covered_s, length_s) / max(covered_s, length_s)
    within_budget = covered_s <= 3 * length_s
    ok = iou_proxy >= 0.3 and within_budget
    detail = {
        "covered_s": covered_s,
        "truth_interval_s": length_s,
        "iou_proxy": iou_proxy,
        "within_3x_budget": within_budget,
        "by_kind": by_kind,
        "approximation": "covered_s is a duration-only proxy for IoU; no per-region positions in the tool schema",
    }
    if ok:
        return "", detail
    return (
        f"marker coverage {covered_s:.2f}s vs truth interval {length_s:.2f}s "
        f"(iou_proxy={iou_proxy:.2f}, within_3x_budget={within_budget})",
        detail,
    )


def check_series_created(outcome):
    created = _parse_list_created(outcome)
    non_marker = [c for c in created if c != "assistant_markers"]
    if not non_marker:
        return "no derived series created (list_created has nothing besides markers)", {"created": created}
    series = outcome.get("series", {}) if isinstance(outcome.get("series"), dict) else {}
    detail = {}
    ok_any = False
    for name in non_marker:
        entry = series.get(name) or {}
        stats = None
        if isinstance(entry, dict) and entry.get("ok"):
            text = entry.get("text")
            try:
                parsed = json.loads(text) if isinstance(text, str) else None
            except json.JSONDecodeError:
                parsed = None
            if isinstance(parsed, dict):
                stats = parsed.get("stats")
        count = stats.get("count") if isinstance(stats, dict) else None
        finite_ok = isinstance(count, (int, float)) and count == count and abs(count) != float("inf") and count > 0
        detail[name] = {"count": count, "finite_ok": finite_ok}
        ok_any = ok_any or finite_ok
    if ok_any:
        return "", detail
    return f"no created series has a finite count>0: {detail}", detail


def check_tab_created(outcome):
    tabs = outcome.get("tab_ids", [])
    if not isinstance(tabs, list):
        tabs = []
    if tabs:
        return "", {"tabs": tabs}
    return "no plot tab found in plot_tab list", {"tabs": tabs}


# ---------------------------------------------------------------------------
# T09: interval_match, sensors_named (optional)
# ---------------------------------------------------------------------------


def _interval_iou(a, b):
    a0, a1 = sorted(a)
    b0, b1 = sorted(b)
    inter = max(0.0, min(a1, b1) - max(a0, b0))
    union = max(a1, b1) - min(a0, b0)
    if union <= 0:
        return 1.0 if a0 == a1 == b0 == b1 else 0.0
    return inter / union


_END_TIME_RE = re.compile(r"(?:end|hasta|to|termina|finaliz\w*)\D{0,12}(-?\d+(?:\.\d+)?)", re.IGNORECASE)


def _extract_model_end_time(anomaly_event, end_events):
    # An explicit anomaly_end event wins; the detail regex is only a fallback
    # (it happily matches "~29-31 L/min" otherwise).
    start = anomaly_event.get("t_s")
    candidates = [
        e.get("t_s")
        for e in end_events
        if isinstance(e.get("t_s"), (int, float)) and (start is None or e["t_s"] >= start)
    ]
    if candidates:
        return min(candidates)
    detail = anomaly_event.get("detail") or ""
    m = _END_TIME_RE.search(detail)
    if m:
        try:
            return float(m.group(1))
        except ValueError:
            pass
    return None


def normalize_relative_times(block, time_axis, offset_s):
    """The prompt asks for PlotJuggler's display axis, which is absolute
    seconds for every dataset here (offset_s > 1e6). A model that answers in
    seconds since the start of the file is still readable by a person, so a
    t_s (or a *_s value) that fits inside [0, duration + 60] on an absolute
    axis is shifted by offset_s and the score records that it happened."""
    if not isinstance(block, dict) or not isinstance(time_axis, dict) or offset_s < 1e6:
        return block, []
    first = time_axis.get("first_sample_abs")
    last = time_axis.get("last_sample_abs")
    if not isinstance(first, (int, float)) or not isinstance(last, (int, float)):
        return block, []
    duration = max(0.0, float(last) - float(first)) + 60.0
    shifted = []
    for ev in block.get("events") or []:
        if isinstance(ev, dict) and isinstance(ev.get("t_s"), (int, float)) and 0 <= ev["t_s"] <= duration:
            shifted.append(f"event {ev.get('kind')}: {ev['t_s']}")
            ev["t_s"] = ev["t_s"] + offset_s
    for name, val in (block.get("values") or {}).items():
        if isinstance(val, dict) and str(name).endswith("_s") and isinstance(val.get("value"), (int, float)):
            if 0 <= val["value"] <= duration:
                shifted.append(f"value {name}: {val['value']}")
                val["value"] = val["value"] + offset_s
    return block, shifted


def check_interval_match_t09(block, truth, time_axis, offset_s):
    te = _truth_fault_event(truth)
    if te is None:
        return "no truth anomaly interval to compare", {}
    t_start = truth_to_display_s(te["t_abs"], time_axis, offset_s)
    tol = te.get("tolerance_s", DEFAULT_TOLERANCE_S)
    t_end_abs = te.get("t_end_abs", te["t_abs"])
    t_end = truth_to_display_s(t_end_abs, time_axis, offset_s)
    model_events = block.get("events", []) if isinstance(block, dict) else []
    anomaly_events = [e for e in model_events if isinstance(e, dict) and e.get("kind") == "anomaly" and isinstance(e.get("t_s"), (int, float))]
    end_events = [e for e in model_events if isinstance(e, dict) and e.get("kind") == "anomaly_end" and isinstance(e.get("t_s"), (int, float))]
    if not anomaly_events:
        return "no model 'anomaly' event with a numeric t_s", {"truth_interval": [t_start, t_end]}
    best = None
    for ae in anomaly_events:
        m_start = ae["t_s"]
        m_end = _extract_model_end_time(ae, end_events)
        if m_end is None:
            m_end = m_start
        iou = _interval_iou((t_start, t_end), (m_start, m_end))
        start_hit = abs(m_start - t_start) <= tol
        candidate = (iou, start_hit, m_start, m_end)
        if best is None or candidate[0] > best[0] or (candidate[0] == best[0] and candidate[1] and not best[1]):
            best = candidate
    iou, start_hit, m_start, m_end = best
    ok = iou >= 0.3 or start_hit
    detail = {
        "iou": iou,
        "start_hit": start_hit,
        "model_interval": [m_start, m_end],
        "truth_interval": [t_start, t_end],
        "tolerance_s": tol,
    }
    if ok:
        return "", detail
    return f"no overlap (IoU={iou:.2f}) and start not within {tol}s of truth", detail


def check_sensors_named(block):
    model_events = block.get("events", []) if isinstance(block, dict) else []
    named = any(
        isinstance(e, dict) and e.get("kind") == "anomaly" and str(e.get("source") or "").strip()
        for e in model_events
    )
    return "" if named else "no anomaly event names a sensor/source"


# ---------------------------------------------------------------------------
# T10 (open): first_sensor (optional-only)
# ---------------------------------------------------------------------------


def _truth_first_deviation_order(truth):
    values = truth.get("values", {}) if isinstance(truth, dict) else {}
    entry = values.get("first_deviation_order")
    if isinstance(entry, list):
        return entry
    if isinstance(entry, dict):
        if isinstance(entry.get("value"), list):
            return entry["value"]
        candidates = entry.get("candidates")
        if isinstance(candidates, list) and candidates:
            c0 = candidates[0]
            if isinstance(c0, dict) and isinstance(c0.get("value"), list):
                return c0["value"]
    return None


def check_first_sensor_t10(block, truth):
    order = _truth_first_deviation_order(truth)
    if not order:
        return "no truth.values.first_deviation_order to compare against"
    top2 = [str(x).strip().lower() for x in order[:2]]
    model_values = block.get("values", {}) if isinstance(block, dict) else {}
    model_events = block.get("events", []) if isinstance(block, dict) else []
    named = None
    for key in ("first_sensor", "first_deviation_sensor"):
        v = model_values.get(key) if isinstance(model_values, dict) else None
        if isinstance(v, dict) and v.get("value"):
            named = str(v["value"]).strip().lower()
            break
    if named is None:
        for e in model_events:
            if isinstance(e, dict) and e.get("source"):
                named = str(e["source"]).strip().lower()
                break
    if named is None:
        return "model did not name a first sensor (checked values.first_sensor/first_deviation_sensor and events[].source)"
    if any(t and (t in named or named in t) for t in top2):
        return ""
    return f"named sensor {named!r} not among truth's first two ({top2})"


# ---------------------------------------------------------------------------
# no_residue (all tasks except T08, only when cleanup ran)
# ---------------------------------------------------------------------------


def check_no_residue(task_id, turn_dir):
    turn_dir = Path(turn_dir)
    cleanup_path = turn_dir / "cleanup.json"
    outcome_path = turn_dir / "outcome.json"
    if not cleanup_path.is_file():
        return "", {"skipped": "cleanup.json not present (cleanup did not run)"}
    if not outcome_path.is_file():
        return "outcome.json missing but cleanup.json present", {}
    try:
        outcome = json.loads(outcome_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "outcome.json unreadable", {}
    created = _parse_list_created(outcome)
    tabs = outcome.get("tab_ids", [])
    if not isinstance(tabs, list):
        tabs = []
    has_residue = bool(created) or bool(tabs)
    detail = {"created": created, "tabs": tabs}
    if task_id == "T03" and has_residue:
        detail["residue_allowed"] = True  # T03's prompt explicitly invites creation
        return "", detail
    if has_residue:
        return f"residue before cleanup: created={created} tabs={tabs}", detail
    return "", detail


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------


def _load_outcome(turn_dir):
    path = Path(turn_dir) / "outcome.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _load_metrics(turn_dir):
    path = Path(turn_dir) / "metrics.json"
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def build_score(cell_dir, truth_dir, task=None, file_override=None, tasks_json_path=None):
    cell_dir = Path(cell_dir)
    parsed = parse_cell_dir(cell_dir)
    file_id = file_override or parsed["file"]
    model = parsed["model"]
    arm = parsed["arm"]
    turns = parsed["turns"]

    if task is None:
        if len(turns) != 1:
            raise ValueError(f"cell has {len(turns)} turn(s) {turns}; pass --task to pick one")
        task = turns[0]
    turn_number = (turns.index(task) + 1) if task in turns else 1
    turn_dir = cell_dir / f"turn{turn_number}"

    tasks_path = Path(tasks_json_path) if tasks_json_path else Path(__file__).resolve().parent / "tasks.json"
    tasks_doc = json.loads(tasks_path.read_text(encoding="utf-8"))
    task_defs = {t["id"]: t for t in tasks_doc.get("tasks", [])}
    task_def = task_defs.get(task, {})
    kinds_table = tasks_doc.get("kinds", {})

    truth = load_truth(truth_dir, task, file_id)
    time_axis = truth.get("time_axis", {})
    offset_s = load_calibration_offset(truth_dir, file_id)

    stream_path = turn_dir / "stream.jsonl"
    records = read_stream_records(stream_path)
    reply = assistant_reply_text(records)
    block, _how = extract_json_block(reply)
    block, relative_shifted = normalize_relative_times(block, time_axis, offset_s)

    structured_reason = ""
    if block is None:
        abort_path = turn_dir / "ABORT.txt"
        if abort_path.is_file():
            structured_reason = "aborted: " + abort_path.read_text(encoding="utf-8", errors="replace").strip()
        else:
            structured_reason = "no_structured_block"

    checks = {}
    required = []
    optional = []

    def add(name, reason, detail=None, required_check=True):
        checks[name] = {"pass": reason == "", "reason": reason, "detail": detail or {}}
        (required if required_check else optional).append(name)

    add("structured_block", structured_reason)

    computed = {}
    metrics = _load_metrics(turn_dir)

    if block is None:
        result = {
            "task": task,
            "file": file_id,
            "model": model,
            "arm": arm,
            "structured_block": False,
            "checks": checks,
            "pass": False,
            "required": required,
            "optional": optional,
            "assumptions": [],
            "tool_use_counts": metrics.get("tool_use_counts", {}),
            "computed": computed,
        }
        return result

    stdin_sent_path = turn_dir / "stdin_sent.txt"
    catalog_text = ""
    if stdin_sent_path.is_file():
        catalog_text = catalog_prefix(stdin_sent_path.read_text(encoding="utf-8", errors="replace"))

    kinds_reason, kinds_detail = check_kinds_valid(block, task, kinds_table)
    add("kinds_valid", kinds_reason, kinds_detail)
    inv_reason, inv_detail = check_no_invented_sources(block, catalog_text, _created_names_from_turn(turn_dir))
    add("no_invented_sources", inv_reason, inv_detail)

    if task == "T01":
        value_names = task_def.get("values", [])
        reason, detail = check_values_within_tolerance(block, truth, time_axis, offset_s, value_names)
        add("values_within_tolerance", reason, detail)
        src_reason, src_detail = check_source_named(block, value_names)
        add("source_named", src_reason, src_detail)
        add("assumptions_present", check_assumptions_present(block), required_check=False)
        computed["value_errors"] = detail

    elif task == "T02":
        reason, detail = check_events_match_t02(block, truth, time_axis, offset_s)
        add("events_match", reason, detail)
        computed["events_match"] = detail

    elif task == "T03":
        reason, detail = check_events_match_t03(block, truth, time_axis, offset_s)
        add("events_match", reason, detail)
        no_res_reason, no_res_detail = check_no_residue(task, turn_dir)
        add("no_residue", no_res_reason, no_res_detail, required_check=False)
        computed["events_match"] = detail

    elif task == "T07":
        t_reason, t_detail = check_fault_time(block, truth, time_axis, offset_s)
        add("fault_time", t_reason, t_detail)
        k_reason, k_detail = check_fault_kind(block, truth, time_axis, offset_s)
        add("fault_kind", k_reason, k_detail)
        add("fault_surface", check_fault_surface(block, truth, t_detail.get("matched_event")), required_check=False)
        add("assumptions_present", check_assumptions_present(block), required_check=False)
        no_res_reason, no_res_detail = check_no_residue(task, turn_dir)
        add("no_residue", no_res_reason, no_res_detail, required_check=False)
        computed["fault_time"] = t_detail
        computed["fault_kind"] = k_detail

    elif task == "T08":
        m_reason, m_detail = check_marker_covers_fault(records, truth, time_axis, offset_s)
        add("marker_covers_fault", m_reason, m_detail)
        outcome = _load_outcome(turn_dir) or {}
        s_reason, s_detail = check_series_created(outcome)
        add("series_created", s_reason, s_detail)
        tab_reason, tab_detail = check_tab_created(outcome)
        add("tab_created", tab_reason, tab_detail)
        computed["marker_covers_fault"] = m_detail

    elif task == "T09":
        reason, detail = check_interval_match_t09(block, truth, time_axis, offset_s)
        add("interval_match", reason, detail)
        add("sensors_named", check_sensors_named(block), required_check=False)
        no_res_reason, no_res_detail = check_no_residue(task, turn_dir)
        add("no_residue", no_res_reason, no_res_detail, required_check=False)
        computed["interval_match"] = detail

    elif task == "T10":
        add("first_sensor", check_first_sensor_t10(block, truth), required_check=False)
        no_res_reason, no_res_detail = check_no_residue(task, turn_dir)
        add("no_residue", no_res_reason, no_res_detail, required_check=False)

    overall_pass = all(checks[n]["pass"] for n in required)
    result = {
        "task": task,
        "file": file_id,
        "model": model,
        "arm": arm,
        "structured_block": True,
        "checks": checks,
        "pass": overall_pass,
        "required": required,
        "optional": optional,
        "assumptions": block.get("assumptions", []) if isinstance(block.get("assumptions"), list) else [],
        "tool_use_counts": metrics.get("tool_use_counts", {}),
        "computed": computed,
    }
    result["relative_time_assumed"] = relative_shifted

    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description="Score one cell of the real-data Assistant Agent benchmark.")
    parser.add_argument("--truth-dir", required=True, help="directory holding truth/<task>[_<file>].json")
    parser.add_argument("--cell", required=True, help="cell directory (contains turn<N>/ subdirectories)")
    parser.add_argument("--task", help="task id, e.g. T07 (default: inferred from the cell dir name / cell.json)")
    parser.add_argument("--file", dest="file_id", help="truth file id, e.g. alfa_03 (default: inferred)")
    parser.add_argument("--tasks-json", help="override path to tasks.json (default: alongside this script)")
    args = parser.parse_args(argv)

    cell_dir = Path(args.cell)
    try:
        result = build_score(cell_dir, args.truth_dir, args.task, args.file_id, args.tasks_json)
    except (FileNotFoundError, ValueError) as exc:
        print(f"verify.py: {exc}", file=sys.stderr)
        return 2

    # Contract (task brief): "<cell_dir>/score.json". A multi-turn cell (e.g.
    # T07+T08 in one conversation) scores each task separately, so a second
    # `--task T08` run on the same cell would silently clobber T07's score if
    # only the bare name were written. Both are written: `score.json` always
    # holds the LAST task scored in this cell (matches the contract literally
    # for the common single-task cell, where the two files are identical),
    # and `score_<task>.json` is the collision-proof one realdata_report.py
    # actually reads. See README "Scoring".
    payload = json.dumps(result, indent=2)
    (cell_dir / "score.json").write_text(payload, encoding="utf-8")
    (cell_dir / f"score_{result['task']}.json").write_text(payload, encoding="utf-8")

    status = "PASS" if result["pass"] else "FAIL"
    failed = [n for n in result["required"] if not result["checks"][n]["pass"]]
    if failed:
        reasons = "; ".join(f"{n}: {result['checks'][n]['reason']}" for n in failed)
    else:
        reasons = "all required checks passed"
    print(f"{status} {result['task']} {result['file']} {result['model']}/{result['arm']}: {reasons}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
