#!/usr/bin/env bash
# Score every finished cell of a run: verify.py per task, file id mapped (flight -> px4).
R=${1:-/home/alvvm/Work/assistant-realdata-bench/runs/2026-09-08}
D=/home/alvvm/Work/pj-official-plugins/.worktrees/assistant/toolbox_assistant_agent/docs/benchmarks/realdata
T=/home/alvvm/Work/assistant-realdata-bench/truth
for c in $(ls -d $R/*-T[0-9]* | xargs -n1 basename); do
  f=$(echo $c | cut -d- -f2); [ "$f" = flight ] && f=px4
  tasks=$(echo $c | cut -d- -f3 | tr '+' ' '); i=0
  for t in $tasks; do i=$((i+1)); [ -f $R/$c/turn$i/done ] || continue
    python3 $D/verify.py --truth-dir $T --cell $R/$c --task $t --file $f 2>&1 | tail -1 | cut -c1-160; done
done
