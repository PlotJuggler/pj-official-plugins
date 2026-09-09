# Rig scripts (reference copy)

These run OUTSIDE the repo, from `/home/alvvm/Work/assistant-realdata-bench/` (absolute paths
inside; adjust `ROOT` if you move the rig). They are kept here so the benchmark can be rebuilt
from the repository alone; datasets are not committed (`compute_truth.py` documents where each
one comes from).

- `start-xephyr.sh` — isolated display `:2` in its own systemd unit (never in the app's unit).
- `run-app.sh <layout> <cell.json>` — staged PlotJuggler with the isolated XDG profile and
  `BENCH_CELL_FILE` for `bench_cli.py`.
- `drive.py <matrix.json>` — walks the cells: restart per layout/arm, wait for the loader's
  "import complete", New chat, type the task id, wait for `done`, screenshot. Resume-safe.
- `score_all.sh [run_dir]` — `verify.py` on every finished cell (`flight` maps to truth id `px4`).
- `notes_audit.py` — per flight: which channels were read, whether the faulty one's note reached the model, verdict.
- `summarize.py` — final tables (pass rates, per-turn cost, blind rubric, at-a-glance split).
- `blind.py` — shuffled grading packet with the model hidden (`BLIND_TASKS`, `BLIND_OUT`,
  `BLIND_PREFIX`).
- `compute_truth.py` — independent ground truth (pyulog / rosbags / pandas); writes the truth JSON
  files and `private_map.json` (the only place the original dataset names appear).
- `axis_calibration.example.json` — display-axis offsets as measured on 2026-09-08: PlotJuggler
  shows absolute seconds (ROS epoch for MCAP, CSV epoch, seconds since boot for ULog).
- `PlotJuggler4.conf.template` — settings seed: `assistant.claude.cli_path` → `bench_cli.py`,
  `assistant.catalog_budget_chars`.
