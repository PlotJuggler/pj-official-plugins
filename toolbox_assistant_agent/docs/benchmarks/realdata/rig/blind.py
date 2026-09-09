#!/usr/bin/env python3
"""Blind grading packet: one markdown per open-task cell, shuffled, model hidden."""
import json, random, sys, glob, os
R = '/home/alvvm/Work/assistant-realdata-bench/runs/2026-09-08'
OPEN = set(os.environ.get('BLIND_TASKS', 'T03,T07,T08,T10').split(','))
def reply_text(stream):
    out = []
    for line in open(stream, encoding='utf-8', errors='replace'):
        try: r = json.loads(line)
        except ValueError: continue
        if r.get('type') == 'assistant':
            for b in r['message'].get('content', []):
                if b.get('type') == 'text': out.append(b['text'])
    return "\n".join(out)
items = []
for cell in sorted(glob.glob(f'{R}/*-T[0-9]*')):
    cid = os.path.basename(cell); parts = cid.split('-')
    ds, f, tasks, model, arm = parts[0], parts[1], parts[2].split('+'), parts[3], parts[4]
    for i, t in enumerate(tasks, start=1):
        if t not in OPEN: continue
        td = f'{cell}/turn{i}'
        if not os.path.exists(f'{td}/done'): continue
        text = reply_text(f'{td}/stream.jsonl')
        # strip the trailing json block (scored by machine) to keep the packet short
        j = text.rfind('```json')
        prose = text[:j] if j > 0 else text
        block = text[j:] if j > 0 else ''
        outcome = {}
        try: outcome = json.load(open(f'{td}/outcome.json'))
        except Exception: pass
        created = ''
        try: created = outcome['list_created']['text']
        except Exception: pass
        tabs = ''
        try: tabs = outcome['plot_tab_list']['text'][:400]
        except Exception: pass
        shots = sorted(glob.glob(f'/home/alvvm/Work/assistant-realdata-bench/artifacts/{cid}-turn{i}*.png'))
        items.append(dict(cid=cid, task=t, file=f, model=model, arm=arm, turn=i, prose=prose[-3500:], block=block[:1500], created=created, tabs=tabs, shots=shots))
random.Random(20260909).shuffle(items)
OUT = os.environ.get('BLIND_OUT', 'blind'); os.makedirs(f'{R}/{OUT}', exist_ok=True)
key = []
for n, it in enumerate(items, start=1):
    bid = os.environ.get('BLIND_PREFIX', 'B') + f'{n:02d}'
    key.append(dict(bid=bid, cid=it['cid'], model=it['model'], task=it['task'], file=it['file'], turn=it['turn']))
    with open(f'{R}/{OUT}/{bid}.md', 'w') as fh:
        fh.write(f"# {bid} — task {it['task']} — file {it['file']} — turn {it['turn']}\n\n")
        fh.write(f"## Created (audit)\n{it['created']}\n\nTabs: {it['tabs']}\n\nScreenshots: {', '.join(it['shots'])}\n\n")
        fh.write(f"## Reply (prose, tail)\n{it['prose']}\n\n## Final block\n{it['block']}\n")
json.dump(key, open(f'{R}/{OUT}/KEY.json', 'w'), indent=1)
print(len(items), 'blind items written to', f'{R}/{OUT}/')
