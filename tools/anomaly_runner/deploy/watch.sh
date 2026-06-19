#!/usr/bin/env bash
# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: Apache-2.0
#
# anomaly-watch — process each NEW MCAP upload through anomaly_runner exactly once,
# letting the runner dispatch notifications (--notify). Dependency-free deploy glue:
# the runner already gates by exit code, so this just tracks which files were done.
#
# Run it on a schedule (cron / systemd.path / inotify) — see the runner README,
# "Deploying the batch watcher". Each invocation scans the dir and processes any
# *.mcap it has not seen before; concurrent runs are de-duped by an atomic marker.
#
#   watch.sh <watch_dir> <rule.json> <notify.json> [source] [runner_path]
#
# State lives under the watch dir:
#   .anomaly_done/<file>.done       — processed marker (delete to re-run a file)
#   .anomaly_reports/<file>.report.json — the JSON report per file

set -euo pipefail

WATCH_DIR=${1:?usage: watch.sh <watch_dir> <rule.json> <notify.json> [source] [runner_path]}
RULE=${2:?need a rule.json (the portable detection rule)}
NOTIFY=${3:?need a notify.json (the notification config)}
SOURCE=${4:-}
RUNNER=${5:-anomaly_runner}

STATE_DIR="$WATCH_DIR/.anomaly_done"
REPORT_DIR="$WATCH_DIR/.anomaly_reports"
mkdir -p "$STATE_DIR" "$REPORT_DIR"

shopt -s nullglob
for f in "$WATCH_DIR"/*.mcap; do
  base=$(basename "$f")
  marker="$STATE_DIR/$base.done"

  # Claim the file atomically: `mkdir` fails if another run already took it, so two
  # overlapping watchers never double-process the same upload.
  if ! mkdir "$marker.lock" 2>/dev/null; then
    continue
  fi
  if [ -e "$marker" ]; then
    rmdir "$marker.lock"
    continue
  fi

  report="$REPORT_DIR/$base.report.json"
  args=(--data "$f" --rule "$RULE" --notify "$NOTIFY" --out "$report")
  [ -n "$SOURCE" ] && args+=(--source "$SOURCE")

  echo "[anomaly-watch] processing $base"
  rc=0
  "$RUNNER" "${args[@]}" || rc=$?
  echo "[anomaly-watch] $base -> exit $rc (report: $report)"

  date -u +"%Y-%m-%dT%H:%M:%SZ exit=$rc" > "$marker"  # mark done (whatever the verdict)
  rmdir "$marker.lock"
done
