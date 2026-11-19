#!/usr/bin/env python3
"""Summarise LeakSanitizer reports: leaks whose stack reaches the repo's own src/ (OWN, file:line of the first
own frame) vs. leaks only in system/third-party libraries (LIB: wx/GTK/fontconfig/GL driver noise).
Usage: leak_summary.py [glob]   (default: <work>/runs_leaks*/*/ubsan.* and <work>/repro_leaks/ubsan.*)"""
import glob, re, collections, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzcfg
SRC = fuzzcfg.REPO + '/src/'
pats = [sys.argv[1]] if len(sys.argv) > 1 else [os.path.join(fuzzcfg.WORK, 'runs_leaks*/*/ubsan.*'),
                                                 os.path.join(fuzzcfg.WORK, 'repro_leaks/ubsan.*')]
agg = collections.Counter(); n = collections.Counter(); runs = collections.defaultdict(set); tot = collections.Counter()
for f in sorted(x for p in pats for x in glob.glob(p)):
    t = open(f, errors='replace').read()
    print(f, re.findall(r'SUMMARY: AddressSanitizer.*', t))
    for b in re.split(r'\n(?=(?:Direct|Indirect) leak)', t):
        m = re.match(r'(Direct|Indirect) leak of (\d+) byte', b)
        if not m: continue
        frames = re.findall(r'#\d+ 0x\w+ in (.+?) (/\S+)', b)
        own = [(fn[:60], loc.replace(fuzzcfg.REPO + '/', '')) for fn, loc in frames if loc.startswith(SRC)]
        if own: key = ('OWN', m.group(1), own[0][1], own[0][0])
        else:
            libs = [re.sub(r'\+0x\w+\)?', '', loc.split('/')[-1]).strip('()') for fn, loc in frames[1:]]
            key = ('LIB', m.group(1), ' > '.join(dict.fromkeys(libs[:3])), '')
        agg[key] += int(m.group(2)); n[key] += 1; runs[key].add(os.path.dirname(f)); tot[key[0]] += int(m.group(2))
print('bytes by class:', dict(tot))
for cls in ('OWN', 'LIB'):
    print('----', cls, '(bytes, blocks, runs, kind, location)')
    for k, v in sorted([x for x in agg.items() if x[0][0] == cls], key=lambda x: -x[1])[:25]: print(v, n[k], len(runs[k]), k[1:])
