#!/usr/bin/env python3
"""Drive a matrix of Assistant Agent benchmark cells through the real app.

Reads `runs/<date>/matrix.json` (a JSON array of cells -- see README.md for
the exact shape) and, for each cell not already `done`, makes sure the right
app instance is running with the right layout+arm, ensures a clean
conversation, types each of the cell's task ids into the panel in order,
waits for bench_cli.py to finish the turn (its `done` marker under
`runs/<date>/<cell-id>/turn<N>/`), screenshots the result, and appends one
line to `runs/<date>/progress.jsonl`.

Resume-safe: re-running with the same matrix skips any cell whose LAST turn
already has a `done` marker.

Stdlib only, plus the external tools already in this rig: xdotool (GUI
driving), pgrep/kill (process management -- never `pkill -f`, see
feedback-pkill-self-match.md), systemd-run (detached app launch, see
bash-background-process-dies.md), and ./shot.sh (screenshots). If
/home/alvvm/Work/assistant-realdata-bench/.venv exists you may run this
under it, but nothing here actually needs a third-party package.

Every X interaction goes through DISPLAY -- ALWAYS ":2" (the rig's Xephyr),
NEVER ":1" (the user's real desktop). See feedback-pathological-windows-xephyr.md.

UNTESTED against the real app: per the task brief, this was written without
running PlotJuggler or the real `claude` CLI. Two things need calibration /
verification on the FIRST live run -- see README.md "First PROBE cell" and
the docstrings on `open_assistant_panel` and `load_ui_coords` below.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path("/home/alvvm/Work/assistant-realdata-bench")
DISPLAY = ":2"
RUN_APP_SH = ROOT / "run-app.sh"
SHOT_SH = ROOT / "shot.sh"
CONF_DIR = ROOT / "profile/config/PlotJuggler"
CONF_PATH = CONF_DIR / "PlotJuggler4.conf"
CONF_TEMPLATE_PATH = CONF_DIR / "PlotJuggler4.conf.template"
UI_COORDS_PATH = ROOT / "profile" / "ui_coords.json"

POLL_INTERVAL_S = 2
DONE_TIMEOUT_S = 20 * 60
WINDOW_WAIT_TIMEOUT_S = 60
APP_START_SETTLE_S = 3
NEW_CHAT_SETTLE_S = 0.5

# assistant.catalog_budget_chars per arm name (matrix.json's "arm" field).
ARM_BUDGETS = {"catalog6000": 6000, "catalogfull": 60000}


# --------------------------------------------------------------------------
# The settings (.conf) file: same INI format as
# assistant-history-e2e/PlotJuggler4.conf.template, edited in place.
# --------------------------------------------------------------------------


def write_conf_key(key, value):
    """Set `key=value` in the [General] section of the real .conf file,
    seeding it from the template first if it doesn't exist yet."""
    if not CONF_PATH.is_file():
        CONF_DIR.mkdir(parents=True, exist_ok=True)
        CONF_PATH.write_text(CONF_TEMPLATE_PATH.read_text(encoding="utf-8"), encoding="utf-8")

    lines = CONF_PATH.read_text(encoding="utf-8").splitlines()
    prefix = key + "="
    in_general = False
    found = False
    out = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_general = stripped == "[General]"
        if in_general and stripped.startswith(prefix):
            out.append(f"{key}={value}")
            found = True
        else:
            out.append(line)

    if not found:
        if any(line.strip() == "[General]" for line in out):
            inserted = []
            done = False
            for line in out:
                inserted.append(line)
                if not done and line.strip() == "[General]":
                    inserted.append(f"{key}={value}")
                    done = True
            out = inserted
        else:
            out = ["[General]", f"{key}={value}"] + out

    CONF_PATH.write_text("\n".join(out) + "\n", encoding="utf-8")


def clear_active_conversation():
    # Same key format the plugin itself writes/reads: conversation_state.cpp,
    # activeSessionIdKey() -> "assistant.conv.<backend>.session_id". Only the
    # "claude" backend is used by this rig.
    write_conf_key("assistant.conv.claude.session_id", "")


def set_catalog_budget(arm):
    write_conf_key("assistant.catalog_budget_chars", str(ARM_BUDGETS.get(arm, 6000)))


# --------------------------------------------------------------------------
# Cell-file / task-turn bookkeeping (read by bench_cli.py via BENCH_CELL_FILE)
# --------------------------------------------------------------------------


def cell_id_for(cell):
    """A filesystem-safe, human-readable id for one matrix row, e.g.
    'px4-sample_log_small-T01-sonnet-catalog6000' or, for a multi-turn cell,
    '...-T07+T08-...'. Used both as the run subdirectory name and in
    screenshot labels."""
    turns = cell.get("turns") or [cell["task"]]
    parts = [cell.get("dataset", "na")]
    file_field = cell.get("file")
    if file_field:
        parts.append(Path(file_field).stem)
    parts.append("+".join(turns))
    parts.append(cell.get("model", "na"))
    if cell.get("arm"):
        parts.append(cell["arm"])
    return "-".join(parts)


def write_active_cell_file(path, cell_run_dir, task_id, cell, turn):
    doc = {
        "run_dir": str(cell_run_dir),
        "task": task_id,
        "model": cell.get("model", "sonnet"),
        "turn": turn,
        "audit": True,
        "cleanup": True,
        "forbidden_words": cell.get("forbidden_words", []),
    }
    path.write_text(json.dumps(doc), encoding="utf-8")


def wait_for_done(turn_dir, timeout_s=DONE_TIMEOUT_S, poll_s=POLL_INTERVAL_S):
    done_path = turn_dir / "done"
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if done_path.is_file():
            return
        time.sleep(poll_s)
    raise TimeoutError(f"{done_path} did not appear within {timeout_s}s")


def rate_limited(turn_dir):
    """True if this turn's stream mentions the usage-window running out
    (claude_backend.cpp's RateLimit handling: any status not starting with
    'allowed')."""
    stream_path = turn_dir / "stream.jsonl"
    if not stream_path.is_file():
        return False
    # rate_limit_event is a FAMILY of records: "allowed" and "allowed_warning"
    # still permit the turn; only a status outside that family, or an error
    # result naming the limit, means the window ran out.
    for line in stream_path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("type") == "rate_limit_event":
            status = str((rec.get("rate_limit_info") or {}).get("status", ""))
            if status and not status.startswith("allowed"):
                return True
        elif rec.get("type") == "result" and rec.get("is_error"):
            text = json.dumps(rec).lower()
            if "rate limit" in text or "usage limit" in text or "rate_limit" in text:
                return True
    return False


def log_progress(progress_path, cell_id, status, **extra):
    progress_path.parent.mkdir(parents=True, exist_ok=True)
    row = {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "cell": cell_id, "status": status}
    row.update(extra)
    with progress_path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row) + "\n")
    print(f"[{row['time']}] {cell_id}: {status}" + (f" {extra}" if extra else ""))


# --------------------------------------------------------------------------
# UI coordinates. Qt widgets have no stable, name-addressable click target
# from the outside, so this rig calibrates pixel coordinates once by hand
# (see README.md "Calibrating profile/ui_coords.json") rather than guessing.
# --------------------------------------------------------------------------


def load_ui_coords():
    if not UI_COORDS_PATH.is_file():
        raise RuntimeError(
            f"missing {UI_COORDS_PATH}. Calibrate it once before the first real run -- "
            f"see README.md 'Calibrating profile/ui_coords.json'."
        )
    doc = json.loads(UI_COORDS_PATH.read_text(encoding="utf-8"))
    for key in ("new_chat_button", "prompt_box"):
        if key not in doc or len(doc[key]) != 2:
            raise RuntimeError(f"{UI_COORDS_PATH} is missing a valid '{key}': [x, y]")
    return doc


# --------------------------------------------------------------------------
# Actions: a real (xdotool/systemd-run/...) implementation and a --dry-run
# one that only prints + still exercises the cell-file/progress-log logic
# above, so the driving LOGIC can be checked without Xephyr or the app.
# --------------------------------------------------------------------------


class RealActions:
    def sh(self, argv, **kwargs):
        env = dict(os.environ)
        env["DISPLAY"] = DISPLAY
        kwargs.setdefault("check", True)
        return subprocess.run(argv, env=env, **kwargs)

    def find_app_pid(self):
        result = subprocess.run(["pgrep", "-x", "plotjuggler4"], capture_output=True, text=True)
        if result.returncode != 0 or not result.stdout.strip():
            return None
        return int(result.stdout.split()[0])

    def kill_app(self, pid, timeout_s=15):
        # Never `pkill -f` (feedback-pkill-self-match.md): an exact PID only.
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.find_app_pid() is None:
                return
            time.sleep(0.5)
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def start_app(self, layout, cell_file):
        # Detached, and NOT reaped at the harness's turn boundary
        # (bash-background-process-dies.md): systemd-run --collect, not a
        # bare `&`.
        unit = f"bench-app-{int(time.time())}"
        self.sh(["systemd-run", "--user", f"--unit={unit}", "--collect", str(RUN_APP_SH), str(layout), str(cell_file)])
        return unit

    def wait_for_import_complete(self, timeout_s=300):
        log_path = ROOT / "runs" / "app.log"
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                text = log_path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                text = ""
            tail = text[text.rfind("=== run-app.sh"):]
            if "import complete" in tail or "Imported" in tail:
                return True
            time.sleep(2)
        print("  warning: no 'import complete' line within the timeout; continuing")
        return False

    def wait_for_window(self, timeout_s=WINDOW_WAIT_TIMEOUT_S):
        try:
            result = self.sh(
                ["xdotool", "search", "--sync", "--onlyvisible", "--name", "PlotJuggler"],
                check=False,
                capture_output=True,
                text=True,
                timeout=timeout_s,
            )
        except subprocess.TimeoutExpired:
            raise TimeoutError("PlotJuggler window did not appear (xdotool search --sync timed out)")
        if result.returncode != 0 or not result.stdout.strip():
            raise TimeoutError(f"xdotool search --sync found no PlotJuggler window: {result.stderr}")
        return result.stdout.strip().splitlines()[-1]

    def click(self, x, y):
        self.sh(["xdotool", "mousemove", "--sync", str(x), str(y)])
        self.sh(["xdotool", "click", "1"])

    def open_assistant_panel(self):
        # Alt+T then Return: reported working in the assistant-history-e2e
        # rig to open the toolbox/Tools menu entry for the Assistant panel.
        # NOT independently re-verified here (this rig's app was never run
        # live during development) -- confirm on the first PROBE cell; if it
        # doesn't land, fall back to a calibrated click on the toolbox tab
        # and add its coordinates to ui_coords.json as "toolbox_tab".
        # Verified live 2026-09-08: without a window manager the mnemonic only
        # lands when addressed to the main window, and the popup needs a
        # moment before Return selects "Assistant Agent" (its only entry).
        wid = self.wait_for_window(timeout_s=10)
        self.sh(["xdotool", "key", "Escape"])
        time.sleep(0.3)
        self.sh(["xdotool", "key", "--window", wid, "alt+t"])
        time.sleep(1.5)
        self.sh(["xdotool", "key", "Return"])
        time.sleep(2.5)

    def click_new_chat(self, coords):
        x, y = coords["new_chat_button"]
        self.click(x, y)
        time.sleep(NEW_CHAT_SETTLE_S)

    def type_task_id(self, task_id, coords):
        x, y = coords["prompt_box"]
        self.click(x, y)
        time.sleep(0.4)
        self.sh(["xdotool", "type", "--clearmodifiers", "--delay", "40", "--", task_id])
        self.sh(["xdotool", "key", "Return"])

    def wait_for_turn_started(self, turn_dir, timeout_s=20):
        """bench_cli.py writes stdin_received.txt the moment the plugin
        invokes it, so its absence after a few seconds means the keystrokes
        did not reach the prompt box (panel not open, focus elsewhere)."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if (Path(turn_dir) / "stdin_received.txt").is_file():
                return True
            time.sleep(1)
        return False

    def switch_to_tab(self, index, coords):
        """Click the Nth plot tab in creation order. Needs 'tab_bar_y',
        'tab_x_start', 'tab_width' calibrated in ui_coords.json -- an
        assumption (tabs lay out left-to-right in creation order at a fixed
        row) that has not been checked against the real app; treat any
        tab-switch screenshot as unverified until you confirm it by eye."""
        if not all(k in coords for k in ("tab_bar_y", "tab_x_start", "tab_width")):
            print(f"  (skipping tab switch for index {index}: tab_bar_y/tab_x_start/tab_width not calibrated)")
            return False
        x = coords["tab_x_start"] + index * coords["tab_width"]
        y = coords["tab_bar_y"]
        self.click(x, y)
        time.sleep(0.5)
        return True

    def take_screenshot(self, label):
        self.sh([str(SHOT_SH), label], check=False)

    def wait_for_done(self, turn_dir):
        wait_for_done(turn_dir)

    def set_catalog_budget(self, arm):
        set_catalog_budget(arm)

    def clear_active_conversation(self):
        clear_active_conversation()


class DryRunActions(RealActions):
    """Touches nothing real: every X/process/systemd/conf-file action just
    prints what it would have done. Only the cell-file write and the
    progress.jsonl log (in process_matrix, not here) are real -- lets the
    matrix-walking LOGIC be checked without Xephyr, the app, the real claude
    CLI, or editing the (possibly live, possibly shared) settings file."""

    def find_app_pid(self):
        print("[dry-run] pgrep -x plotjuggler4 -> (assume not running)")
        return None

    def kill_app(self, pid, timeout_s=15):
        print(f"[dry-run] would kill PID {pid}")

    def start_app(self, layout, cell_file):
        print(f"[dry-run] would: systemd-run --user --unit=bench-app-... --collect run-app.sh {layout} {cell_file}")
        return "dry-run-unit"

    def wait_for_window(self, timeout_s=WINDOW_WAIT_TIMEOUT_S):
        print("[dry-run] would: xdotool search --sync --onlyvisible --name PlotJuggler")
        return "dry-run-window-id"

    def open_assistant_panel(self):
        print("[dry-run] would: xdotool key alt+t, Return (open Assistant panel)")

    def wait_for_turn_started(self, turn_dir, timeout_s=20):
        print(f"[dry-run] would wait for {turn_dir}/stdin_received.txt")
        return True

    def wait_for_import_complete(self, timeout_s=300):
        print("[dry-run] would wait for 'import complete' in runs/app.log")
        return True

    def click_new_chat(self, coords):
        print(f"[dry-run] would click New chat at {coords.get('new_chat_button')}")

    def type_task_id(self, task_id, coords):
        print(f"[dry-run] would click prompt box at {coords.get('prompt_box')}, type {task_id!r}, press Return")

    def switch_to_tab(self, index, coords):
        print(f"[dry-run] would switch to tab index {index}")
        return True

    def take_screenshot(self, label):
        print(f"[dry-run] would: ./shot.sh {label}")

    def wait_for_done(self, turn_dir):
        print(f"[dry-run] would poll for {turn_dir}/done every {POLL_INTERVAL_S}s (timeout {DONE_TIMEOUT_S}s)")

    def set_catalog_budget(self, arm):
        print(f"[dry-run] would set assistant.catalog_budget_chars for arm {arm!r} in {CONF_PATH}")

    def clear_active_conversation(self):
        print(f"[dry-run] would clear assistant.conv.claude.session_id in {CONF_PATH}")


def load_dry_run_ui_coords():
    if UI_COORDS_PATH.is_file():
        return json.loads(UI_COORDS_PATH.read_text(encoding="utf-8"))
    return {"new_chat_button": [0, 0], "prompt_box": [0, 0]}


# --------------------------------------------------------------------------
# Main matrix walk
# --------------------------------------------------------------------------


def process_matrix(matrix_path, actions, coords):
    matrix_path = Path(matrix_path).resolve()  # absolute: the app unit and bench_cli.py have other cwds
    cells = json.loads(matrix_path.read_text(encoding="utf-8"))
    run_root = matrix_path.parent
    progress_path = run_root / "progress.jsonl"
    active_cell_file = run_root / "active_cell.json"
    active_cell_file.parent.mkdir(parents=True, exist_ok=True)

    running_layout = None
    running_arm = None

    for cell in cells:
        cid = cell_id_for(cell)
        cell_run_dir = run_root / cid
        turns = cell.get("turns") or [cell["task"]]
        last_done = cell_run_dir / f"turn{len(turns)}" / "done"
        if last_done.is_file():
            log_progress(progress_path, cid, "skipped_already_done")
            continue

        needs_new_instance = running_layout != cell["layout"] or running_arm != cell.get("arm")
        if needs_new_instance:
            pid = actions.find_app_pid()
            if pid is not None:
                actions.kill_app(pid)
            actions.set_catalog_budget(cell.get("arm"))
            actions.clear_active_conversation()
            write_active_cell_file(active_cell_file, cell_run_dir, turns[0], cell, turn=1)
            actions.start_app(cell["layout"], active_cell_file)
            actions.wait_for_window()
            # A big MCAP (alfa_02: 87k messages) is still importing when the
            # window appears; keystrokes sent then go nowhere and the cell is
            # lost. Wait for the loader's "import complete" line written after
            # this launch's marker in runs/app.log.
            actions.wait_for_import_complete()
            time.sleep(APP_START_SETTLE_S)
            actions.open_assistant_panel()
            running_layout = cell["layout"]
            running_arm = cell.get("arm")
        else:
            actions.click_new_chat(coords)
            write_active_cell_file(active_cell_file, cell_run_dir, turns[0], cell, turn=1)

        stop_after_cell = False
        for i, task_id in enumerate(turns, start=1):
            if i > 1:
                write_active_cell_file(active_cell_file, cell_run_dir, task_id, cell, turn=i)

            turn_dir = cell_run_dir / f"turn{i}"
            actions.type_task_id(task_id, coords)
            if not actions.wait_for_turn_started(turn_dir):
                # One retry: reopen the panel (idempotent) and type again.
                print(f"  {cid} turn {i}: prompt did not reach the plugin; reopening the panel and retrying once")
                actions.open_assistant_panel()
                actions.type_task_id(task_id, coords)
                if not actions.wait_for_turn_started(turn_dir):
                    log_progress(progress_path, cid, "prompt_not_delivered", turn=i)
                    stop_after_cell = True
                    break

            try:
                actions.wait_for_done(turn_dir)
            except TimeoutError as exc:
                log_progress(progress_path, cid, "timeout", turn=i, error=str(exc))
                stop_after_cell = True
                break

            if i == 1:
                invocation_path = turn_dir / "invocation.json"
                if invocation_path.is_file():
                    invocation = json.loads(invocation_path.read_text(encoding="utf-8"))
                    if invocation.get("resume_present"):
                        log_progress(progress_path, cid, "contaminated_turn1_resumed", turn=i)
                        print(
                            f"STOPPING: {cid} turn 1 carried --resume -- New chat did not take effect. "
                            f"Check ui_coords.json's new_chat_button coordinate."
                        )
                        return

            actions.take_screenshot(f"{cid}-turn{i}")

            outcome_path = turn_dir / "outcome.json"
            if outcome_path.is_file():
                try:
                    outcome = json.loads(outcome_path.read_text(encoding="utf-8"))
                except json.JSONDecodeError:
                    outcome = {}
                for tab_index, tab_id in enumerate(outcome.get("tab_ids", [])):
                    if actions.switch_to_tab(tab_index, coords):
                        actions.take_screenshot(f"{cid}-turn{i}-tab-{tab_id}")

            if rate_limited(turn_dir):
                log_progress(progress_path, cid, "rate_limited", turn=i)
                print(f"Rate limit hit at {cid} turn {i}. Resume later with the same matrix -- already-done cells are skipped.")
                return

        if stop_after_cell:
            continue

        log_progress(progress_path, cid, "done")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", help="path to runs/<date>/matrix.json")
    parser.add_argument("--dry-run", action="store_true", help="log the plan without touching X, the app, or claude")
    args = parser.parse_args()

    if args.dry_run:
        actions = DryRunActions()
        coords = load_dry_run_ui_coords()
    else:
        actions = RealActions()
        coords = load_ui_coords()

    process_matrix(args.matrix, actions, coords)


if __name__ == "__main__":
    main()
