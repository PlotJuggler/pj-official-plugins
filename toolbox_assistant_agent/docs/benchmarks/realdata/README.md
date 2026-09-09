# Real-data benchmark: RUN side

This directory holds the pieces that make the Assistant Agent panel run against
a real `claude` CLI while a wrapper records everything about the turn. It is
the RUN half of the real-data benchmark described in the approved plan
(`buenas-vamos-ac-ontinuar-sharded-simon.md`); scoring (`verify.py`) and the
report generator (`realdata_report.py`) are the SCORING half, documented in
"Scoring" below.

Two locations are involved:

- **This directory** (in the plugin repo): `bench_cli.py` (the wrapper CLI),
  `tasks.json` (given -- the fixed task prompts and their `id`s), `truth/`
  (given -- ground truth for scoring, not used by the RUN side).
- **The rig**, `/home/alvvm/Work/assistant-realdata-bench/` (outside this
  repo): `drive.py` (the GUI driver), `run-app.sh` (launches the staged app),
  `profile/config/PlotJuggler/PlotJuggler4.conf.template` (settings template),
  `staged/` (built app + plugin, prepared separately), `datasets/`,
  `layouts/`, `runs/` (all output).

## How a turn actually runs

The Assistant Agent panel's `assistant.claude.cli_path` setting points at
`bench_cli.py` instead of the real `claude` binary. The plugin invokes it
exactly as it would invoke `claude` (src/claude_backend.cpp:29-76): same argv
shape, the turn's payload on stdin, `stream-json` NDJSON read back from
stdout. `bench_cli.py` sits in the middle:

```
plugin -> bench_cli.py -> real claude (child process) -> bench_cli.py -> plugin
```

Control comes from the `BENCH_CELL_FILE` environment variable, which
`run-app.sh` sets for the whole app process (so every turn of every
conversation that instance runs sees it). `drive.py` rewrites the file's
*contents* between turns/cells -- the env var's value (a path) never changes
while the app is running.

Without `BENCH_CELL_FILE` pointing at a real file, `bench_cli.py` is a
transparent proxy to the real CLI (`BENCH_REAL_CLAUDE`, default `claude`):
same argv, inherited stdio, nothing recorded. This is what makes it safe to
point a normal, non-benchmark settings file at this script by accident.

## Cell file format (`BENCH_CELL_FILE`)

```json
{
  "run_dir": "/home/alvvm/Work/assistant-realdata-bench/runs/2026-09-08/px4-sample_log_small-T01-sonnet-catalog6000",
  "task": "T01",
  "model": "sonnet",
  "turn": 1,
  "audit": true,
  "cleanup": true,
  "forbidden_words": ["engine_failure", "control_surface_fault"]
}
```

- `run_dir`: where this cell's `turn<N>/` output directories go.
- `task`: a `tasks.json` id (e.g. `"T01"`), or the literal string `"PROBE"`
  (see below). `bench_cli.py` only substitutes the id if the LAST block of
  stdin (after the last blank line -- see `split_payload` in `bench_cli.py`)
  equals a known task id; anything else is forwarded as typed, so a driver
  bug that types free text still runs, just without substitution.
- `model`: forced into `--model` regardless of what the plugin itself passed
  (its own `--model` is either replaced or appended).
- `turn`: 1-based turn number in this conversation; picks the
  `run_dir/turn<N>/` output directory.
- `audit` / `cleanup`: see "What lands on disk" below.
- `forbidden_words`: dataset-truth words (e.g. the ALFA fault label) that must
  never appear in the catalog part of the payload -- see "Invariant 2" below.

## What lands on disk, per turn

`run_dir/turn<N>/`:

| File | Written when | Contents |
|---|---|---|
| `stdin_received.txt` | always | exact stdin bytes from the plugin |
| `stdin_sent.txt` | always | what was actually sent to the real CLI (task id substituted, if it matched) |
| `ABORT.txt` | leak check tripped | the forbidden word found; **no model call happened** |
| `invocation.json` | model was called | argv, `resume_present`, `--mcp-config` path + parsed URL (never the bearer token), model, task, turn |
| `stream.jsonl` | model was called | the real CLI's NDJSON stream, byte-identical to what was forwarded to our own stdout |
| `stderr.txt` | model was called | the real CLI's stderr |
| `metrics.json` | model was called | wall time, exit code, assistant-message count, tool_use counts by name, and the `result` record's `session_id`/`num_turns`/`duration_ms`/`duration_api_ms`/`is_error`/`usage` (all four token counts) |
| `outcome.json` | `audit: true` | `list_created`, `plot_tab {action:list}`, `report_status`, and per-created-series `read_series` stats (tried as `<name>/value` first, since that's how `plot_tab` addresses a created series, falling back to `<name>`) |
| `cleanup.json` | `audit: true` and `cleanup: true` | `remove_markers`, `remove_derived_series` per created name, `plot_tab close` per listed tab |
| `bench_cli_error.txt` | an exception during audit/cleanup | full traceback -- **never changes the exit code**, which is always the real CLI's own |
| `done` | always, last | the exit code, as text -- `drive.py` polls for this file's existence |

## Invariants

1. **Turn 1 of a fresh conversation never carries `--resume`.** `drive.py`
   checks `invocation.json`'s `resume_present` after turn 1 and marks the
   cell `contaminated_turn1_resumed` (stopping the whole run) if it does --
   that means "New chat" didn't actually take effect before typing.
2. **The catalog never contains a forbidden word.** Checked by `bench_cli.py`
   itself, against the payload's catalog part (everything before the LAST
   blank-line-separated block, which is the user's own text --
   `harness_memory.hpp:50-78`), before the real CLI is ever started. A hit
   writes `ABORT.txt` and exits 1 with no model call.

## PROBE mode: host-latency sonde, no model

Set the cell's `"task": "PROBE"`. `bench_cli.py` does not start the real CLI
at all; instead it uses the same `--mcp-config` the plugin gave it to call the
live MCP server directly (`tools/list`, `list_topics`, `describe_topic` on up
to 32 topics to find the longest series, then `read_series` stats/buckets,
`evaluate`, `create_derived_series` + `read_series` on the result, then
`remove_derived_series`), times each call, and writes `run_dir/probe.json`.
It still emits a minimal, CLI-shaped NDJSON stream (system/init, one
assistant text message summarizing the timings, a zero-usage `result`) so the
panel has something sane to render. Exits 0 even if the sonde itself fails
(the error lands in `probe.json`, not as a crash) -- PROBE never touches the
model, so a broken MCP endpoint here is a rig problem, not a scored turn.

### Running the first PROBE cell (once `staged/` is complete)

Nothing above has been run against the real app or the real `claude` CLI --
per the task brief, this was built without launching either. Do this by hand
first, in order:

```bash
R=/home/alvvm/Work/assistant-realdata-bench

# 1. Seed the real settings file from the template (only needed once).
mkdir -p "$R/profile/config/PlotJuggler"
cp "$R/profile/config/PlotJuggler/PlotJuggler4.conf.template" \
   "$R/profile/config/PlotJuggler/PlotJuggler4.conf"

# 2. Write a PROBE cell file (a real layout must already be loaded by it).
mkdir -p "$R/runs/2026-09-08/probe-px4"
cat > "$R/runs/2026-09-08/probe.json" << 'EOF'
{"run_dir":"/home/alvvm/Work/assistant-realdata-bench/runs/2026-09-08/probe-px4","task":"PROBE","model":"sonnet","turn":1}
EOF

# 3. Launch (foreground first, to watch it -- systemd-run --collect for a
#    detached run once this works).
"$R/run-app.sh" "$R/layouts/<a px4 layout>.pj4.xml" "$R/runs/2026-09-08/probe.json"

# 4. In the running app: open the Assistant panel (try Alt+T then Return --
#    see the caveat in drive.py's open_assistant_panel docstring; if that
#    doesn't focus it, click the toolbox tab by hand this once), click the
#    prompt box, type PROBE, press Return.

# 5. Watch it land:
cat "$R/runs/2026-09-08/probe-px4/probe.json"
```

If that works end to end, the same cell file mechanism drives real T01..T10
cells the same way -- calibrate `profile/ui_coords.json` (below) next, then
hand a real matrix to `drive.py`.

## Calibrating `profile/ui_coords.json`

`drive.py` clicks the panel's "New chat" button and prompt box by pixel
coordinate -- Qt widgets have no other stable, name-addressable click target
from outside the process. Calibrate once, after step 3 above, with the app
running and the Assistant panel open:

```bash
DISPLAY=:2 "$R/shot.sh" calibrate
```

Open `$R/artifacts/calibrate.png` in any image viewer that shows pixel
coordinates on hover, and note:

- the center of the **New chat** button (`assistant_panel.ui`'s
  `newChatButton`, top-right of the "Conversations" drawer header)
- the center of the **prompt box** (`inputEdit`, the text field at the
  bottom, left of the Send button)

Write them to `$R/profile/ui_coords.json`:

```json
{"new_chat_button": [X1, Y1], "prompt_box": [X2, Y2]}
```

Optional, for the `plot_tab` screenshot step (only needed for tasks that
create tabs, e.g. T08): `tab_bar_y`, `tab_x_start`, `tab_width` -- `drive.py`
clicks tab N (in `outcome.json`'s `tab_ids` creation order) at
`(tab_x_start + N * tab_width, tab_bar_y)`. This assumes tabs lay out
left-to-right in creation order at one fixed row, which has not been checked
against the real app -- verify the first tab-switch screenshot by eye.

## Matrix format (`runs/<date>/matrix.json`)

A JSON array of cells:

```json
[
  {
    "dataset": "px4",
    "file": "sample_log_small.ulg",
    "layout": "/home/alvvm/Work/assistant-realdata-bench/layouts/px4.pj4.xml",
    "task": "T01",
    "model": "sonnet",
    "arm": "catalog6000",
    "turns": ["T01"],
    "forbidden_words": []
  },
  {
    "dataset": "alfa",
    "file": "alfa_01.mcap",
    "layout": "/home/alvvm/Work/assistant-realdata-bench/layouts/alfa_01.pj4.xml",
    "task": "T07",
    "model": "opus",
    "arm": "catalog6000",
    "turns": ["T07", "T08"],
    "forbidden_words": ["engine_failure", "control_surface_fault"]
  }
]
```

`turns` is the ordered list of task ids to run in ONE conversation (a
multi-turn cell like T07 then T08); when absent, `drive.py` treats it as
`[task]`. `arm` selects `assistant.catalog_budget_chars` (`catalog6000` ->
6000, `catalogfull` -> 60000) and, together with `layout`, decides whether
`drive.py` must restart the app (different layout or arm) or can reuse the
running instance with just a fresh conversation (same layout+arm, "New chat"
between cells).

## Running a matrix

```bash
cd /home/alvvm/Work/assistant-realdata-bench
python3 drive.py runs/2026-09-08/matrix.json          # real run
python3 drive.py runs/2026-09-08/matrix.json --dry-run  # log the plan only, no X/app/claude
```

`--dry-run` exercises the exact same cell-walking logic (instance
restart/reuse decisions, cell-file writes, multi-turn sequencing,
resume-skip) without touching Xephyr, the app, or the real CLI -- useful to
sanity-check a new matrix before spending real usage-window budget on it.

Resume-safe: re-running with the same matrix skips any cell whose LAST turn
already has a `run_dir/turn<N>/done` file. If a turn's stream mentions the
usage window running out (`rate_limit` / `usage limit`), `drive.py` stops and
prints the same command to resume with later.

## Where output lands

- `runs/<date>/matrix.json` -- the matrix (given).
- `runs/<date>/<cell-id>/turn<N>/` -- everything `bench_cli.py` wrote (see
  table above). `<cell-id>` is `<dataset>-<file-stem>-<turns joined with
  '+'>-<model>-<arm>`.
- `runs/<date>/active_cell.json` -- the single, constantly-rewritten cell file
  `BENCH_CELL_FILE` actually points at.
- `runs/<date>/progress.jsonl` -- one line per cell (`done`,
  `skipped_already_done`, `timeout`, `rate_limited`,
  `contaminated_turn1_resumed`).
- `runs/app.log` -- the app's own stdout/stderr, appended across every
  `run-app.sh` invocation.
- `artifacts/<label>.png` -- screenshots (`./shot.sh <label>`; the driver
  labels them `<cell-id>-turn<N>` and `<cell-id>-turn<N>-tab-<tab-id>`).

## Self-tests (offline, no real app or real claude)

`bench_cli.py` was verified against a fake `claude`
(`runs/_selftest/fake_claude.py`, kept in the rig): `--version`, task-id
substitution into `stdin_sent.txt`, `--model` override from the cell
(replacing OR appending), byte-identical stream forwarding, exit-code
propagation, the leak-check abort, PROBE mode's graceful failure when the MCP
endpoint is unreachable, and that an unreachable `--mcp-config` during
audit/cleanup lands in `bench_cli_error.txt` without changing the exit code.
`drive.py`'s cell-walking logic (restart-vs-reuse, multi-turn sequencing,
resume-skip) was checked with `--dry-run` against a synthetic 3-cell matrix.
Neither the real PlotJuggler app nor the real `claude` CLI was run.

# Scoring

`verify.py` scores one task's turn against ground truth and writes
`score.json`; `realdata_report.py` aggregates every cell under one
`runs/<date>/` into a Markdown report. Both are Python 3 stdlib only, and
neither touches `bench_cli.py` or `tasks.json`.

## Running verify.py

On one cell (task and file inferred from the cell dir name, or from a
`cell.json` inside it if the driver ever writes one):

```bash
python3 verify.py --truth-dir truth/ --cell runs/2026-09-08/px4-sample_log_small-T01-sonnet-catalog6000
```

A multi-turn cell (e.g. the T07→T08 conversation) scores one task per
invocation — pass `--task` to pick which turn:

```bash
python3 verify.py --truth-dir truth/ --cell runs/2026-09-08/alfa-alfa_03-T07+T08-opus-catalogfull --task T07
python3 verify.py --truth-dir truth/ --cell runs/2026-09-08/alfa-alfa_03-T07+T08-opus-catalogfull --task T08
```

Each run writes **two** files next to the cell: `score.json` (the contract's
literal name — always the LAST task scored in that cell) and
`score_<task>.json` (task-qualified, collision-proof). For a single-task cell
they're identical; for a multi-turn cell, `score.json` alone would get
silently overwritten by the second `--task` run, so `realdata_report.py`
always reads the qualified file (falling back to the bare one only if the
qualified file is missing, e.g. an older run scored before this existed).

## Running it over a whole run

`verify.py` scores one cell at a time; to score every cell under a date,
loop over the cell directories and pass each one's inferred task(s):

```bash
for cell in runs/2026-09-08/*/; do
  for task in $(python3 -c "
import sys; sys.path.insert(0, '.')
import verify
print(' '.join(verify.parse_cell_dir(sys.argv[1])['turns']))
" "$cell"); do
    python3 verify.py --truth-dir truth/ --cell "$cell" --task "$task"
  done
done
python3 realdata_report.py runs/2026-09-08/ > runs/2026-09-08/report.md
```

## Check list per task

`""` (empty reason) means pass; every check's `detail` is stored in
`score.json` regardless of pass/fail. Optional checks never affect
`score.json`'s top-level `pass`.

| Task | Required checks | Optional checks |
|---|---|---|
| all tasks | `structured_block`, `kinds_valid` (only tasks with a `tasks.json["kinds"]` entry), `no_invented_sources` | — |
| T01 | + `values_within_tolerance`, `source_named` | `assumptions_present` |
| T02 | + `events_match` (≥80% of truth matched, ≤1 spurious) | — |
| T03 (open) | + `events_match` (truth interval hit, or `none`/no events when truth has none) | `no_residue` |
| T07 | + `fault_time`, `fault_kind` | `fault_surface`, `assumptions_present`, `no_residue` |
| T08 (turn 2, follows T07) | + `marker_covers_fault`, `series_created`, `tab_created` | — (no `no_residue`: residue is the point of the task) |
| T09 | + `interval_match` (IoU ≥ 0.3 or start within tolerance) | `sensors_named`, `no_residue` |
| T10 (open) | `structured_block`, `kinds_valid` (task has no `kinds` entry, so a no-op), `no_invented_sources` | `first_sensor` (only check that matters for this task) |

`no_residue` applies to every task except T08, and only when `cleanup.json`
exists (cleanup actually ran); for T03 the prompt explicitly invites creating
a series/markers, so residue there is recorded (`checks.no_residue.detail`)
rather than failed.

## Axis calibration

Truth times are absolute, in the file's own clock (`time_axis.kind`:
`ros_epoch`, `ulog_us_since_boot`, or `csv_datetime_utc`); the model answers
in PlotJuggler's display-axis seconds. One function does the conversion
everywhere (`verify.truth_to_display_s`):

```
t_display = (t_abs - first_sample_abs) * scale + offset_s
```

`scale` is `1e-6` for `ulog_us_since_boot` (PX4 ULog microseconds-since-boot),
`1` otherwise. `offset_s` comes from `truth/axis_calibration.json`:

```json
{"alfa_03": {"offset_s": 0.0}}
```

keyed by the truth file id (the `_<file>` part of `truth/<task>_<file>.json`,
e.g. `alfa_03`), defaulting to `0.0` when the file or the key is missing — a
later manual calibration step fills these in once the real recordings and
their display-axis start are compared by hand.

## Marker coverage (T08) — a documented limitation

`create_markers`'s tool result (`src/tools.cpp:62-119`,
`publishedMarkerSummary`) reports `markers_created`, `by_kind` (counts) and
`covered_s` (the UNION duration of region markers) — it never reports
individual region start/end timestamps. `marker_covers_fault` therefore
cannot compute a true IoU against the truth interval; it uses
`iou_proxy = min(covered_s, L) / max(covered_s, L)` (`L` = truth interval
length) as an optimistic upper bound — it equals the true IoU only if the
marked region(s) happen to be optimally aligned, and cannot detect a
correctly-sized but misplaced marker. The `≤ 3× truth interval length` budget
check is exact (it only needs `covered_s`), and still catches the "marker
covers the whole flight" failure mode. `score.json`'s
`checks.marker_covers_fault.detail.approximation` always states this caveat.

## Self-tests

```bash
python3 tests_verify.py
```

Builds synthetic cell directories (fake `stream.jsonl`/`metrics.json`/
`outcome.json`/`cleanup.json`/`stdin_sent.txt`) and synthetic truth files in a
temp directory, and asserts BOTH directions of every check above: a correct
reply passes, and a wrong one (time off by 3× tolerance, wrong fault kind, a
missing JSON block, an invented source, a marker covering the whole flight
instead of the fault stretch, ...) fails with the expected reason. Also
covers the ULog µs-since-boot axis conversion, the calibration-offset file,
cell-dir-name parsing (including the `cell.json` override), and an
end-to-end multi-turn (T07→T08) cell scored through `build_score`.

## Assumptions worth flagging

- **`kind` values ending in `_end`** (e.g. `anomaly_end` for T09's interval
  end) are accepted by `kinds_valid` even though `tasks.json["kinds"]` only
  lists the start kind — the task brief describes both an `anomaly_end`
  event and an end embedded in `detail` as valid ways to state an interval's
  end, which would otherwise collide with a strict kind list.
- **`no_invented_sources` matching is prefix-based**, against topic paths
  parsed out of the catalog digest (`catalogDigest` in `src/tools.cpp`):
  first the full `source` path, then progressively shorter prefixes, down to
  the topic itself. When the catalog is truncated or names-only (its footer
  says so), only the first path segment is required to match, since an
  unseen topic beyond the truncation cutoff cannot be ruled out.
- **T01's time-valued names** (`takeoff_s`, `landing_s`) are detected by a
  `_s` suffix rather than a hardcoded name list, so the axis conversion
  applies to any future time-shaped value with the same naming convention.
- **T08's truth file** is looked up as `truth/T08_<file>.json` (not
  `T07_<file>.json`), per the given `truth/<task>[_<file>].json` naming
  pattern, even though T08 "follows" T07 in the same fault — the two truth
  files are expected to describe the same underlying interval.
- **T10's "first sensor"** has no fixed field in the shared JSON schema (only
  `events`/`values`/`assumptions`), so `first_sensor` checks, in order:
  `values.first_sensor.value`, `values.first_deviation_sensor.value`, then
  the `source` of the first event — against the first two names in
  `truth.values.first_deviation_order` (accepting either a plain list or the
  `{"value": [...]}` / `{"candidates": [{"value": [...]}]}` shapes truth's
  other fields use).
