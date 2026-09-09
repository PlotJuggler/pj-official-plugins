#!/usr/bin/env bash
# Start the staged PlotJuggler4 (with the Assistant Agent plugin) under Xephyr
# :2, wired to bench_cli.py via BENCH_CELL_FILE. Modeled on
# /home/alvvm/Work/assistant-history-e2e/run-final.sh, adapted for this rig.
#
# Usage: run-app.sh <layout-file> <cell-file>
#
# <layout-file> MUST be passed with --layout: a positional path is silently
# ignored by plotjuggler4 (verified in the assistant-history-e2e report).
# <cell-file> becomes BENCH_CELL_FILE in the app's own environment -- every
# turn of every conversation this app instance runs sees it, so switching
# cells means rewriting the file's content or restarting the app (drive.py's
# job), not restarting this script mid-cell.
set -euo pipefail

ROOT=/home/alvvm/Work/assistant-realdata-bench

usage() {
  echo "usage: run-app.sh <layout-file> <cell-file>" >&2
  exit 2
}

LAYOUT=${1:?$(usage)}
CELL_FILE=${2:?$(usage)}

test -x "$ROOT/staged/bin/plotjuggler4" || { echo "missing $ROOT/staged/bin/plotjuggler4 -- stage the app first" >&2; exit 2; }
test -f "$ROOT/staged/plugins/libtoolbox_assistant_agent_plugin.so" || {
  echo "missing the assistant plugin under $ROOT/staged/plugins -- stage it first" >&2
  exit 2
}
test -f "$LAYOUT" || { echo "layout file not found: $LAYOUT" >&2; exit 2; }
test -f "$CELL_FILE" || { echo "cell file not found: $CELL_FILE" >&2; exit 2; }

"$ROOT/start-xephyr.sh"

CONF_DIR="$ROOT/profile/config/PlotJuggler"
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/PlotJuggler4.conf" ]; then
  # First run only: seed the real settings file from the template. Later runs
  # keep whatever drive.py already edited into it (model/arm/session-id
  # bookkeeping) -- this script must never clobber that.
  cp "$ROOT/profile/config/PlotJuggler/PlotJuggler4.conf.template" "$CONF_DIR/PlotJuggler4.conf"
fi

BENCH_CLI=/home/alvvm/Work/pj-official-plugins/.worktrees/assistant/toolbox_assistant_agent/docs/benchmarks/realdata/bench_cli.py
test -f "$BENCH_CLI" || { echo "bench_cli.py not found at $BENCH_CLI" >&2; exit 2; }
# Not exported: the plugin never reads this. It only matters that the conf
# file's assistant.claude.cli_path points at it -- checked here as a sanity
# guard before we spend a Xephyr+app launch on a broken path.

REAL_CLAUDE="$(command -v claude || true)"
if [ -z "$REAL_CLAUDE" ]; then
  echo "warning: 'claude' not found on PATH -- BENCH_REAL_CLAUDE left as 'claude' (transparent-proxy calls will fail loudly)" >&2
  REAL_CLAUDE="claude"
fi

export DISPLAY=:2
export XDG_CONFIG_HOME="$ROOT/profile/config"
export XDG_DATA_HOME="$ROOT/profile/data"
export XDG_CACHE_HOME="$ROOT/profile/cache"
export XDG_STATE_HOME="$ROOT/profile/state"
export BENCH_CELL_FILE="$CELL_FILE"
export BENCH_REAL_CLAUDE="$REAL_CLAUDE"
unset QT_IM_MODULE

mkdir -p "$ROOT/runs"
printf '\n=== run-app.sh %s (layout=%s cell=%s) ===\n' "$(date --iso-8601=seconds)" "$LAYOUT" "$CELL_FILE" >> "$ROOT/runs/app.log"

exec "$ROOT/staged/bin/plotjuggler4" --plugin-dir "$ROOT/staged/plugins" --layout "$LAYOUT" \
  >> "$ROOT/runs/app.log" 2>&1
