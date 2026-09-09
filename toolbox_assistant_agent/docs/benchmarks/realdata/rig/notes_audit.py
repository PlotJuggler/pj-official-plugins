#!/usr/bin/env python3
"""Per flight: which servo channels the model read, whether the faulty channel's
flat_span/constant note reached it, and the verdict. Separates 'did not look'
from 'looked and misread'. Usage: notes_audit.py <run_dir>"""
import json, glob, os, sys, re
run = sys.argv[1]
ann = json.load(open('/home/alvvm/Work/assistant-realdata-bench/truth/glance/annotations.json'))
pm = json.load(open('/home/alvvm/Work/assistant-realdata-bench/truth/private_map.json')).get('alfa', {})
# faulty channel(s) per flight from the independent flat-run computation (rc/out), cached below
FAULTY = {'alfa_02': [5], 'alfa_03': [5], 'alfa_05': [4, 5], 'alfa_06': [4, 5], 'alfa_10': [3, 4],
          'alfa_12': [4], 'alfa_01': [3], 'alfa_17': [3], 'alfa_15': [1], 'alfa_16': [1]}
rows = []
for sf in sorted(glob.glob(f'{run}/alfa-*-sonnet-*/score_T07.json')):
    d = os.path.dirname(sf); f = json.load(open(sf))['file'].split('/')[-1]; ok = json.load(open(sf))['pass']
    calls = {}; read_ch = set(); note_seen = set(); const_seen = set()
    for line in open(f'{d}/turn1/stream.jsonl'):
        try: r = json.loads(line)
        except ValueError: continue
        if r.get('type') == 'assistant':
            for b in r['message'].get('content', []):
                if b.get('type') == 'tool_use': calls[b['id']] = b
        elif r.get('type') == 'user':
            for b in r['message'].get('content', []):
                if b.get('type') != 'tool_result' or b['tool_use_id'] not in calls: continue
                c = b.get('content'); t = c[0].get('text', '') if isinstance(c, list) and c else ''
                try: dd = json.loads(t)
                except ValueError: continue
                for e in (dd.get('read') or [dd]):
                    m = re.search(r'rc/out/channels\[(\d+)\]', e.get('series', ''))
                    if not m: continue
                    ch = int(m.group(1)); read_ch.add(ch); st = e.get('stats', {})
                    if 'flat_span_note' in st or 'flat_span_s' in st: note_seen.add(ch)
                    if st.get('constant'): const_seen.add(ch)
    faulty = FAULTY.get(f, []); kind = pm.get(f, {}).get('kind', '?')
    looked = all(ch in read_ch for ch in faulty) if faulty else None
    saw = all(ch in note_seen for ch in faulty) if faulty else None
    rows.append((f, ann.get(f, {}).get('glance', '?'), kind, 'PASS' if ok else 'FAIL', sorted(read_ch), faulty, looked, saw, sorted(const_seen)))
print(f"{'file':8s} {'glance':8s} {'truth':22s} {'res':5s} {'channels read':22s} {'faulty':8s} {'looked':7s} {'note?':6s} constant-flagged")
for r in rows:
    print(f"{r[0]:8s} {r[1]:8s} {r[2][:22]:22s} {r[3]:5s} {str(r[4]):22s} {str(r[5]):8s} {str(r[6]):7s} {str(r[7]):6s} {r[8]}")
