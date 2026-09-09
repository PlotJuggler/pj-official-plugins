#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/alvvm/Work/assistant-realdata-bench
mkdir -p "$ROOT/artifacts"
if xdpyinfo -display :2 >/dev/null 2>&1; then
  echo "Xephyr display :2 is already ready"
  exit 0
fi
# Own transient unit: if Xephyr lived in the app unit's cgroup, killing the app
# (drive.py restarts it per layout) would take the display down with it.
systemd-run --user --unit="bench-xephyr-$(date +%s)" --collect -p Environment=DISPLAY=:1 \
  Xephyr :2 -screen 1600x1000 -ac -noreset >"$ROOT/artifacts/xephyr.log" 2>&1
pgrep -x Xephyr | tail -1 > "$ROOT/artifacts/xephyr.pid"
for _ in $(seq 1 40); do
  if xdpyinfo -display :2 >/dev/null 2>&1; then
    echo "Xephyr display :2 ready (PID $(cat "$ROOT/artifacts/xephyr.pid"))"
    exit 0
  fi
  sleep 0.1
done
echo "Xephyr did not become ready; see $ROOT/artifacts/xephyr.log" >&2
exit 1
