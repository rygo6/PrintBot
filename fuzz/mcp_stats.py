#!/usr/bin/env python3
"""Count MCP call outcomes in a fuzz campaign: ok / error / refused by the re-entrancy guard (ui.busy,
ui.blocked_by_dialog), split by whether the fuzzer cleared dialogs first. Usage: mcp_stats.py <tag>"""
import glob, json, sys, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg
tag = sys.argv[1] if len(sys.argv) > 1 else 'main'
c = collections.Counter()
for f in glob.glob(os.path.join(fuzzcfg.WORK, f'runs_{tag}/*/actions.jsonl')):
    pend = {}
    for l in open(f):
        try: r = json.loads(l)
        except Exception: continue
        if r['k'] == 'mcp': pend = r
        elif r['k'] == 'mcp_res':
            t = r['txt']; guard = ('ui.blocked_by_dialog' in t or 'ui.busy' in t)
            kind = 'mutating_cleared' if pend.get('paused') else 'other'
            c[(kind, 'guard' if guard else ('err' if r['err'] else 'ok'))] += 1
        elif r['k'] == 'mcp_timeout': c['timeout'] += 1
print(dict(c))
