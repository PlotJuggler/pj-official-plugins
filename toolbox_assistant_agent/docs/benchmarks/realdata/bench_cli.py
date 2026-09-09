#!/usr/bin/env python3
"""Benchmark wrapper installed as the Assistant Agent's `assistant.claude.cli_path`.

The plugin invokes this exactly as it would invoke the real `claude` CLI (see
src/claude_backend.cpp:29-76 in this repo): same argv shape, the turn's
payload on stdin, stdout read as `stream-json` NDJSON. This wrapper sits
between the plugin and the real CLI:

  plugin -> bench_cli.py -> real claude (child process) -> bench_cli.py -> plugin

It does three things the plugin cannot do on its own:
  1. Substitutes a fixed task id (e.g. "T01") appearing as the turn's user
     text for that task's full prompt, read from tasks.json -- so the driver
     only ever has to type a short id into the panel.
  2. Records everything about the turn (argv, stdin, the raw stream, derived
     metrics) to `<run_dir>/turn<N>/`.
  3. After the real CLI exits, optionally audits what got created (via the
     same MCP endpoint the model used) and cleans it up, so the next turn --
     or the next cell -- starts from a known state.

Control is entirely out-of-band, via the `BENCH_CELL_FILE` environment
variable (set by run-app.sh into the app's own environment, so every turn of
every conversation sees the same file unless the driver rewrites it between
turns). Without it -- e.g. if someone points a normal PlotJuggler settings
file at this script by accident -- this is a transparent proxy to the real
CLI: same argv, inherited stdio, nothing recorded.

Python 3 stdlib only: this runs as the app's CLI, not under any project venv.
"""
import json
import re
import os
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.request
from pathlib import Path

VERSION_STRING = "bench-cli 1.0"

# MCP tool calls run on the GUI thread with a 60s host-side timeout
# (src/gui_executor.hpp:31); give the HTTP round trip a bit more headroom.
MCP_TIMEOUT_S = 65


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)
    sys.stderr.flush()


# --------------------------------------------------------------------------
# MCP client (same protocol as deterministic_cli.py in assistant-history-e2e)
# --------------------------------------------------------------------------


def mcp_rpc(url, token, request_id, method, params=None):
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=MCP_TIMEOUT_S) as response:
        return json.loads(response.read().decode("utf-8"))


class McpClient:
    """Thin JSON-RPC client against the plugin's loopback MCP server."""

    def __init__(self, url, token):
        self.url = url
        self.token = token
        self._next_id = 1

    def call(self, name, arguments):
        # Deliberately does NOT catch transport errors (unreachable MCP,
        # timeout, ...): the audit/cleanup call site in main() wraps its
        # whole block in one try/except and logs to bench_cli_error.txt,
        # which is where an unreachable MCP endpoint belongs -- not silently
        # folded into a per-call "ok": false that would hide it in outcome.json.
        request_id = self._next_id
        self._next_id += 1
        reply = mcp_rpc(self.url, self.token, request_id, "tools/call", {"name": name, "arguments": arguments})
        result = reply.get("result", {}) if isinstance(reply, dict) else {}
        if not isinstance(result, dict):
            result = {}
        content = result.get("content", [])
        text = content[0].get("text", "") if content else ""
        return {"ok": not result.get("isError", False), "text": text, "raw": result}

    def initialize(self):
        try:
            mcp_rpc(
                self.url,
                0,
                "initialize",
                {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "bench-cli", "version": "1.0"}},
            )
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            eprint(f"bench_cli: MCP initialize failed: {exc}")


def parse_tool_json(text):
    try:
        return json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return None


def truncated(value, limit=2048):
    """A JSON-safe, size-bounded copy of `value` for probe.json."""
    dumped = json.dumps(value)
    if len(dumped) <= limit:
        return value
    return {"_truncated": True, "text": dumped[:limit]}


# --------------------------------------------------------------------------
# Cell / task-file plumbing
# --------------------------------------------------------------------------


def load_cell(cell_file):
    return json.loads(Path(cell_file).read_text(encoding="utf-8"))


def load_tasks_by_id(script_dir):
    tasks_path = script_dir / "tasks.json"
    if not tasks_path.is_file():
        return {}
    try:
        doc = json.loads(tasks_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}
    out = {}
    for task in doc.get("tasks", []):
        task_id = task.get("id")
        if task_id:
            out[task_id] = task
    return out


def split_payload(stdin_received):
    """Split `catalog[...]\\n\\nuser_text` into (prefix_incl_sep, last_block).

    `prefix_incl_sep` is "" when there was no "\\n\\n" at all (a resumed turn
    with nothing new to prepend -- harness_memory.hpp:56-57 sends just the
    user text in that case). The LAST "\\n\\n" is always the boundary right
    before the user's own text (harness_memory.hpp:50-78): composePayload
    never puts a blank line inside the catalog+note block after that point,
    so this is exact as long as the user text itself has no "\\n\\n" in it --
    true for every payload this rig sends (a bare task id).
    """
    if "\n\n" in stdin_received:
        prefix, _, last_block = stdin_received.rpartition("\n\n")
        return prefix + "\n\n", last_block
    return "", stdin_received


def rewrite_model_flag(flags, model):
    """Return a copy of `flags` with --model set to `model` (replaced or appended)."""
    flags = list(flags)
    if "--model" in flags:
        idx = flags.index("--model")
        if idx + 1 < len(flags):
            flags[idx + 1] = model
        else:
            flags.append(model)
    elif model:
        flags += ["--model", model]
    return flags


def find_flag_value(flags, flag):
    if flag in flags:
        idx = flags.index(flag)
        if idx + 1 < len(flags):
            return flags[idx + 1]
    return None


def read_mcp_config(mcp_config_path):
    """Return (url, token) from a --mcp-config file, or (None, None)."""
    if not mcp_config_path:
        return None, None
    try:
        cfg = json.loads(Path(mcp_config_path).read_text(encoding="utf-8"))
        server = cfg["mcpServers"]["pj"]
        url = server["url"]
        auth = server["headers"]["Authorization"]
        token = auth[len("Bearer "):] if auth.startswith("Bearer ") else None
        return url, token
    except (OSError, json.JSONDecodeError, KeyError, TypeError):
        return None, None


# --------------------------------------------------------------------------
# Audit / cleanup (after the real CLI has exited)
# --------------------------------------------------------------------------


def run_audit(client, turn_dir):
    outcome = {
        "list_created": client.call("list_created", {}),
        "plot_tab_list": client.call("plot_tab", {"action": "list"}),
        "report_status": client.call("report_status", {}),
    }

    created_ids = []
    parsed = parse_tool_json(outcome["list_created"]["text"])
    if isinstance(parsed, dict) and isinstance(parsed.get("created"), list):
        created_ids = [c for c in parsed["created"] if isinstance(c, str)]

    series = {}
    for created_id in created_ids:
        if created_id == "assistant_markers":
            continue  # a marker set, not a readable series
        last = None
        for candidate_path in (f"{created_id}/value", created_id):
            reply = client.call("read_series", {"paths": [candidate_path], "mode": "stats"})
            last = {"path": candidate_path, **reply}
            if reply["ok"]:
                break
        series[created_id] = last
    outcome["series"] = series
    outcome["created_ids"] = created_ids

    tab_ids = []
    parsed_tabs = parse_tool_json(outcome["plot_tab_list"]["text"])
    if isinstance(parsed_tabs, dict) and isinstance(parsed_tabs.get("tabs"), list):
        for entry in parsed_tabs["tabs"]:
            if isinstance(entry, dict) and isinstance(entry.get("tab"), str):
                tab_ids.append(entry["tab"])
    outcome["tab_ids"] = tab_ids

    (turn_dir / "outcome.json").write_text(json.dumps(outcome, indent=2), encoding="utf-8")
    return outcome


def run_cleanup(client, outcome, turn_dir):
    cleanup = {"remove_markers": client.call("remove_markers", {})}

    removed_series = {}
    for created_id in outcome.get("created_ids", []):
        if created_id == "assistant_markers":
            continue
        removed_series[created_id] = client.call("remove_derived_series", {"name": created_id})
    cleanup["remove_derived_series"] = removed_series

    closed_tabs = {}
    for tab_id in outcome.get("tab_ids", []):
        closed_tabs[tab_id] = client.call("plot_tab", {"action": "close", "tab": tab_id})
    cleanup["plot_tab_close"] = closed_tabs

    (turn_dir / "cleanup.json").write_text(json.dumps(cleanup, indent=2), encoding="utf-8")
    return cleanup


# --------------------------------------------------------------------------
# PROBE mode: host-latency sonde, no model involved
# --------------------------------------------------------------------------


def run_probe(cell, run_dir, mcp_url, mcp_token):
    timings = {}
    responses = {}

    def timed_call(label, name, arguments):
        start = time.time()
        reply = client.call(name, arguments)
        timings[label] = time.time() - start
        responses[label] = truncated(reply)
        return reply

    if not mcp_url or not mcp_token:
        (run_dir / "probe.json").write_text(
            json.dumps({"error": "no usable --mcp-config for PROBE mode"}, indent=2), encoding="utf-8"
        )
        return emit_probe_stream(cell, {"error": "no usable --mcp-config"})

    client = McpClient(mcp_url, mcp_token)
    client.initialize()

    try:
        # tools/list is a distinct JSON-RPC method, not a tools/call.
        start = time.time()
        raw = mcp_rpc(mcp_url, mcp_token, 0, "tools/list")
        tools_list = {"ok": True, "text": json.dumps(raw.get("result", {}))}
        timings["tools_list"] = time.time() - start
        responses["tools_list"] = truncated(tools_list)

        list_topics = timed_call("list_topics", "list_topics", {"limit": 500})
        topics_doc = parse_tool_json(list_topics["text"]) or {}
        topic_names = [t.get("topic") for t in topics_doc.get("topics", []) if isinstance(t, dict) and t.get("topic")]

        # Find the longest series: describe up to 32 topics to get one field
        # path each, then read_series stats on each candidate, keeping the max
        # count.
        candidates = []
        # Prefer high-rate sensor topics so the probe hits the biggest series the
        # dataset has (a PX4 log lists 500+ topics; the first 32 by name are
        # rarely the IMU ones).
        prefer = re.compile(r'sensor_combined|imu|accel|gyro|vehicle_attitude|vfr_hud|nav_info|Accelerometer|Volume', re.I)
        ordered = [t for t in topic_names if prefer.search(t)] + [t for t in topic_names if not prefer.search(t)]
        for topic in ordered[:32]:
            described = client.call("describe_topic", {"topic": topic})
            doc = parse_tool_json(described["text"]) or {}
            fields = doc.get("fields", [])
            if fields and isinstance(fields[0], dict) and fields[0].get("path"):
                candidates.append(fields[0]["path"])

        best_path = None
        best_count = -1
        candidate_stats = {}
        probe_start = time.time()
        for path in candidates:
            reply = client.call("read_series", {"paths": [path], "mode": "stats"})
            doc = parse_tool_json(reply["text"]) or {}
            count = doc.get("stats", {}).get("count", -1) if isinstance(doc.get("stats"), dict) else -1
            candidate_stats[path] = count
            if reply["ok"] and count > best_count:
                best_count = count
                best_path = path
        timings["find_longest_series"] = time.time() - probe_start
        responses["candidate_series_counts"] = candidate_stats
        responses["longest_series"] = best_path

        if best_path is None:
            probe_doc = {"timings": timings, "responses": responses, "error": "no readable series found"}
            (run_dir / "probe.json").write_text(json.dumps(probe_doc, indent=2), encoding="utf-8")
            return emit_probe_stream(cell, probe_doc)

        timed_call("read_series_stats", "read_series", {"paths": [best_path], "mode": "stats"})
        timed_call("read_series_buckets", "read_series", {"paths": [best_path], "mode": "buckets", "max_points": 500})
        timed_call("evaluate", "evaluate", {"inputs": [best_path], "expression": "value * 2", "buckets": 100})
        timed_call(
            "create_derived_series",
            "create_derived_series",
            {"name": "bench/probe", "inputs": [best_path], "expression": "value * 2"},
        )
        timed_call("read_series_created_stats", "read_series", {"paths": ["bench/probe/value"], "mode": "stats"})
        timed_call("remove_derived_series", "remove_derived_series", {"name": "bench/probe"})

        probe_doc = {
            "longest_series": best_path,
            "longest_series_count": best_count,
            "timings": timings,
            "responses": responses,
        }
        (run_dir / "probe.json").write_text(json.dumps(probe_doc, indent=2), encoding="utf-8")
        return emit_probe_stream(cell, probe_doc)
    except Exception:
        # PROBE never calls the model; a broken/unreachable MCP endpoint here
        # is a rig problem, not a turn failure, so it gets a clean report
        # instead of a bare traceback.
        error_text = traceback.format_exc()
        probe_doc = {"timings": timings, "responses": responses, "error": error_text}
        (run_dir / "probe.json").write_text(json.dumps(probe_doc, indent=2), encoding="utf-8")
        return emit_probe_stream(cell, {"error": "PROBE raised an exception, see probe.json", "timings": timings})


def emit_probe_stream(cell, probe_doc):
    """Print a minimal, CLI-shaped NDJSON stream so the panel renders something
    sane for a PROBE cell (which never calls the model)."""
    session_id = "bench-probe"
    summary_lines = ["PROBE: host-latency sonde (no model call)."]
    if "error" in probe_doc:
        summary_lines.append(f"error: {probe_doc['error']}")
    else:
        summary_lines.append(f"longest series: {probe_doc.get('longest_series')} (count={probe_doc.get('longest_series_count')})")
        for label, seconds in probe_doc.get("timings", {}).items():
            summary_lines.append(f"{label}: {seconds:.3f}s")
    summary = "\n".join(summary_lines)

    def emit(record):
        line = json.dumps(record, separators=(",", ":"))
        print(line, flush=True)

    emit({"type": "system", "subtype": "init", "session_id": session_id})
    emit(
        {
            "type": "assistant",
            "session_id": session_id,
            "message": {"content": [{"type": "text", "text": summary}]},
        }
    )
    emit(
        {
            "type": "result",
            "subtype": "success",
            "is_error": False,
            "session_id": session_id,
            "num_turns": 1,
            "result": summary,
            "total_cost_usd": 0.0,
            "duration_ms": 0,
            "duration_api_ms": 0,
            "usage": {
                "input_tokens": 0,
                "output_tokens": 0,
                "cache_read_input_tokens": 0,
                "cache_creation_input_tokens": 0,
            },
        }
    )
    return 0


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------


def proxy_to_real_claude(real_claude, flags):
    """Transparent proxy: replace this process with the real CLI, inheriting
    stdio untouched. Never returns on success."""
    os.execvp(real_claude, [real_claude] + flags)
    # execvp only returns on failure.
    eprint(f"bench_cli: cannot exec real claude '{real_claude}'")
    return 127


def main(argv):
    flags = argv[1:]

    if "--version" in flags:
        print(VERSION_STRING)
        return 0

    real_claude = os.environ.get("BENCH_REAL_CLAUDE", "claude")
    cell_file = os.environ.get("BENCH_CELL_FILE")

    if not cell_file or not Path(cell_file).is_file():
        return proxy_to_real_claude(real_claude, flags)

    try:
        cell = load_cell(cell_file)
    except (OSError, json.JSONDecodeError) as exc:
        eprint(f"bench_cli: cannot read cell file '{cell_file}': {exc}; proxying to real claude")
        return proxy_to_real_claude(real_claude, flags)

    run_dir = Path(cell["run_dir"])
    turn = int(cell.get("turn", 1))
    turn_dir = run_dir / f"turn{turn}"
    turn_dir.mkdir(parents=True, exist_ok=True)

    stdin_received = sys.stdin.read()
    (turn_dir / "stdin_received.txt").write_text(stdin_received, encoding="utf-8")

    script_dir = Path(__file__).resolve().parent
    tasks_by_id = load_tasks_by_id(script_dir)

    prefix, last_block = split_payload(stdin_received)
    task_id_candidate = last_block.strip()
    replaced = task_id_candidate in tasks_by_id
    if replaced:
        last_block = tasks_by_id[task_id_candidate]["prompt_full"]
    stdin_sent = prefix + last_block
    (turn_dir / "stdin_sent.txt").write_text(stdin_sent, encoding="utf-8")

    # Leak check: forbidden dataset-truth words must never appear in what we
    # are about to send as the catalog (`prefix`, everything before the
    # user's own text). Checked against stdin_sent's prefix, which is
    # identical to stdin_received's here -- only the trailing user-text block
    # is ever substituted.
    forbidden_words = cell.get("forbidden_words") or []
    if forbidden_words:
        lowered_catalog = prefix.lower()
        hit = next((w for w in forbidden_words if w.lower() in lowered_catalog), None)
        if hit is not None:
            (turn_dir / "ABORT.txt").write_text(
                f"forbidden word found in catalog before calling the model: {hit!r}\n", encoding="utf-8"
            )
            return 1

    mcp_config_path = find_flag_value(flags, "--mcp-config")
    mcp_url, mcp_token = read_mcp_config(mcp_config_path)

    if cell.get("task") == "PROBE":
        code = run_probe(cell, run_dir, mcp_url, mcp_token)
        (turn_dir / "done").write_text(str(code), encoding="utf-8")
        return code

    model = cell.get("model", "")
    new_flags = rewrite_model_flag(flags, model)
    resume_present = "--resume" in new_flags
    child_argv = [real_claude] + new_flags

    invocation = {
        "argv": child_argv,
        "resume_present": resume_present,
        "resume_session_id": find_flag_value(new_flags, "--resume"),
        "mcp_config_path": mcp_config_path,
        "mcp_url": mcp_url,
        "model": model,
        "task": cell.get("task"),
        "task_placeholder_replaced": replaced,
        "run_dir": str(run_dir),
        "turn": turn,
    }
    (turn_dir / "invocation.json").write_text(json.dumps(invocation, indent=2), encoding="utf-8")

    stderr_path = turn_dir / "stderr.txt"
    stream_path = turn_dir / "stream.jsonl"

    start = time.time()
    try:
        stderr_f = open(stderr_path, "wb")
        stream_f = open(stream_path, "wb")
    except OSError as exc:
        eprint(f"bench_cli: cannot open output files in {turn_dir}: {exc}")
        return 1

    result_record = None
    assistant_message_count = 0
    tool_use_counts = {}
    exit_code = 1
    try:
        try:
            proc = subprocess.Popen(child_argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr_f)
        except OSError as exc:
            eprint(f"bench_cli: cannot start real claude '{real_claude}': {exc}")
            return 127

        try:
            proc.stdin.write(stdin_sent.encode("utf-8"))
        except (BrokenPipeError, OSError):
            pass
        finally:
            try:
                proc.stdin.close()
            except OSError:
                pass

        for raw_line in proc.stdout:
            stream_f.write(raw_line)
            stream_f.flush()
            sys.stdout.buffer.write(raw_line)
            sys.stdout.flush()

            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            record_type = record.get("type")
            if record_type == "assistant":
                assistant_message_count += 1
                content = record.get("message", {}).get("content", [])
                if isinstance(content, list):
                    for block in content:
                        if isinstance(block, dict) and block.get("type") == "tool_use":
                            name = block.get("name", "unknown")
                            tool_use_counts[name] = tool_use_counts.get(name, 0) + 1
            elif record_type == "result":
                result_record = record

        exit_code = proc.wait()
    finally:
        stderr_f.close()
        stream_f.close()

    wall_s = time.time() - start

    metrics = {
        "wall_s": wall_s,
        "exit_code": exit_code,
        "assistant_message_count": assistant_message_count,
        "tool_use_counts": tool_use_counts,
    }
    if result_record is not None:
        usage = result_record.get("usage", {})
        if not isinstance(usage, dict):
            usage = {}
        metrics["result"] = {
            "session_id": result_record.get("session_id"),
            "num_turns": result_record.get("num_turns"),
            "duration_ms": result_record.get("duration_ms"),
            "duration_api_ms": result_record.get("duration_api_ms"),
            "is_error": result_record.get("is_error"),
            "usage": {
                "input_tokens": usage.get("input_tokens"),
                "output_tokens": usage.get("output_tokens"),
                "cache_read_input_tokens": usage.get("cache_read_input_tokens"),
                "cache_creation_input_tokens": usage.get("cache_creation_input_tokens"),
            },
        }
    (turn_dir / "metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")

    # Audit + cleanup must never change the exit code we already have from the
    # real CLI: any exception here is logged, not raised.
    try:
        if cell.get("audit") and mcp_url and mcp_token:
            client = McpClient(mcp_url, mcp_token)
            client.initialize()
            outcome = run_audit(client, turn_dir)
            if cell.get("cleanup"):
                run_cleanup(client, outcome, turn_dir)
    except Exception:
        (turn_dir / "bench_cli_error.txt").write_text(traceback.format_exc(), encoding="utf-8")

    (turn_dir / "done").write_text(str(exit_code), encoding="utf-8")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
