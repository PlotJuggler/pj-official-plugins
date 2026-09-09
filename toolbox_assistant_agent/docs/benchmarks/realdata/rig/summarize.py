#!/usr/bin/env python3
"""Final tables for the real-data benchmark run: pass rates, per-turn cost, rubric."""
import json, glob, os, statistics as st
from collections import defaultdict
R='/home/alvvm/Work/assistant-realdata-bench/runs/2026-09-08'
def med(xs): return st.median(xs) if xs else float('nan')
# --- pass rates by (model, task)
passes=defaultdict(lambda: [0,0]); cells=[]
for sc in glob.glob(f'{R}/*/score_T*.json'):
    s=json.load(open(sc)); k=(s.get('model'), s['task']); passes[k][1]+=1; passes[k][0]+=int(bool(s['pass']))
    cells.append(s)
print("## Acierto por modelo y tarea (verify.py)\n")
print("| tarea | sonnet | opus |\n|---|---|---|")
for t in sorted({k[1] for k in passes}):
    row=[]
    for m in ('sonnet','opus'):
        p,n=passes.get((m,t),[0,0]); row.append(f"{p}/{n}" if n else "—")
    print(f"| {t} | {row[0]} | {row[1]} |")
# --- per turn cost by model and dataset
turns=defaultdict(list)
for mf in glob.glob(f'{R}/*/turn*/metrics.json'):
    cell=os.path.basename(os.path.dirname(os.path.dirname(mf))); parts=cell.split('-'); ds,model=parts[0],parts[3]
    if 'PROBE' in cell: continue
    m=json.load(open(mf)); r=m.get('result') or {}; u=r.get('usage') or {}
    if not u: continue
    turns[(model,ds)].append(dict(wall=m['wall_s'], msgs=m['assistant_message_count'], tools=sum(m['tool_use_counts'].values()), out=u.get('output_tokens',0), cr=u.get('cache_read_input_tokens',0), cc=u.get('cache_creation_input_tokens',0)))
    turns[(model,'todos')].append(turns[(model,ds)][-1])
print("\n## Coste por turno (medianas; tokens tal cual los reporta el CLI)\n")
print("| modelo | dataset | turnos | s/turno | mensajes | llamadas | tokens salida | cache leída | cache escrita |\n|---|---|---|---|---|---|---|---|---|")
for (model,ds),xs in sorted(turns.items(), key=lambda kv:(kv[0][0],kv[0][1]!='todos',kv[0][1])):
    print(f"| {model} | {ds} | {len(xs)} | {med([x['wall'] for x in xs]):.0f} | {med([x['msgs'] for x in xs]):.0f} | {med([x['tools'] for x in xs]):.0f} | {med([x['out'] for x in xs]):.0f} | {med([x['cr'] for x in xs]):.0f} | {med([x['cc'] for x in xs]):.0f} |")
# --- rubric
rows=[]
for pack in ('blind','blind_t10'):
    key={k['bid']:k for k in json.load(open(f'{R}/{pack}/KEY.json'))}
    g=json.load(open(f'{R}/{pack}/grades.json'))
    for bid,v in g.items():
        if bid.startswith('_'): continue
        k=key[bid]; rows.append(dict(model=k['model'],task=k['task'],art=v['artefacto'],hon=v['honestidad']))
agg=defaultdict(lambda: {'art':[], 'hon':[]})
for r in rows:
    a=agg[(r['model'],r['task'])]; a['hon'].append(r['hon'])
    if r['art'] is not None: a['art'].append(r['art'])
print("\n## Rúbrica ciega (Fable; artefacto 0-2 solo cuando creó algo, honestidad 0-2)\n")
print("| modelo | tarea | ítems | artefacto media (n) | honestidad media |\n|---|---|---|---|---|")
for (m,t),a in sorted(agg.items()):
    print(f"| {m} | {t} | {len(a['hon'])} | {st.mean(a['art']):.2f} ({len(a['art'])}) | {st.mean(a['hon']):.2f} |" if a['art'] else f"| {m} | {t} | {len(a['hon'])} | — (0) | {st.mean(a['hon']):.2f} |")
# --- glance vs correctness for T07
ann=json.load(open('/home/alvvm/Work/assistant-realdata-bench/truth/glance/annotations.json'))
print("\n## T07 por visibilidad a simple vista (acierto de verify.py)\n")
print("| a simple vista | vuelos | sonnet | opus |\n|---|---|---|---|")
byg=defaultdict(lambda: defaultdict(lambda:[0,0]))
files=set()
for s in cells:
    if s['task']!='T07': continue
    f=s['file'].split('/')[-1]; gl=ann.get(f,{}).get('glance','?'); files.add((gl,f))
    byg[gl][s['model']][1]+=1; byg[gl][s['model']][0]+=int(bool(s['pass']))
for gl in ('obvious','partial','none'):
    n=len([1 for g,f in files if g==gl]); so=byg[gl]['sonnet']; op=byg[gl]['opus']
    print(f"| {gl} | {n} | {so[0]}/{so[1]} | {op[0]}/{op[1]} |")
