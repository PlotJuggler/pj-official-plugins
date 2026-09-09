#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/alvvm/Work/assistant-realdata-bench
LABEL=${1:?usage: shot.sh LABEL}
case "$LABEL" in *[!A-Za-z0-9._-]*) echo "label contains unsafe characters" >&2; exit 2;; esac
export DISPLAY=:2
WINDOW=$(xdotool search --onlyvisible --name 'PlotJuggler' | tail -1)
xwd -silent -id "$WINDOW" -out "$ROOT/artifacts/$LABEL.xwd"
ffmpeg -loglevel error -y -i "$ROOT/artifacts/$LABEL.xwd" "$ROOT/artifacts/$LABEL.png"
echo "$ROOT/artifacts/$LABEL.png"
