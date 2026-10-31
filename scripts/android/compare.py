#!/usr/bin/env python3
# Usage: compare.py A.csv B.csv -- per (case,mode) geomean of C drift, Rust change, and Rust/C ratio
import csv, math, sys
from collections import defaultdict
def load(f):
    return {(r['case'], r['mode'], r['quality']): r for r in csv.DictReader(open(f))}
a, b = load(sys.argv[1]), load(sys.argv[2])
g = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0, 0])
for k in a:
    if k not in b: continue
    for key in (k[1], 'ALL', k[0][:24]):
        e = g[key]
        e[0] += math.log(float(b[k]['c_dec_mibs']) / float(a[k]['c_dec_mibs']))
        e[1] += math.log(float(b[k]['rust_dec_mibs']) / float(a[k]['rust_dec_mibs']))
        e[2] += math.log(float(a[k]['rust_dec_mibs']) / float(a[k]['c_dec_mibs']))
        e[3] += math.log(float(b[k]['rust_dec_mibs']) / float(b[k]['c_dec_mibs']))
        e[4] += 1
print(f"{'group':26} {'C drift':>8} {'Rust chg':>8} {'R/C A':>8} {'R/C B':>8}")
for key, e in sorted(g.items()):
    n = e[4]
    f = lambda x: f"{(math.exp(x / n) - 1) * 100:+7.1f}%"
    print(f"{key:26} {f(e[0])} {f(e[1])} {f(e[2])} {f(e[3])}")
