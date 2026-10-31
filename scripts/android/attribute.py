#!/usr/bin/env python3
# Usage: attribute.py BINARY ADDRFILE SYMBOL_SUBSTR OUTER_SUBSTR
# Per-instruction listing of SYMBOL with sample %, inner source line, and the
# call-site line in the outermost frame containing OUTER_SUBSTR; then a
# per-outer-line histogram.
import re, subprocess, sys
from collections import defaultdict
binp, addrf, sym, outer = sys.argv[1:5]
pct = {}
for line in open(addrf):
    m = re.match(r'\s*([\d.]+)%\s+0x([0-9a-f]+)', line)
    if m:
        pct[int(m.group(2), 16)] = float(m.group(1))
nm = subprocess.run(['arm-linux-gnueabihf-nm', '-S', '--defined-only', binp], capture_output=True, text=True).stdout
cands = [(int(p[1], 16), int(p[0], 16) & ~1) for p in (l.split() for l in nm.splitlines()) if len(p) == 4 and sym in p[3]]
size, start = max(cands, key=lambda c: sum(v for a, v in pct.items() if c[1] <= a < c[1] + c[0]))
dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-d', '--no-show-raw-insn', f'--start-address={start:#x}',
                      f'--stop-address={start+size:#x}', binp], capture_output=True, text=True).stdout
rows = []
for l in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
    if m:
        rows.append((int(m.group(1), 16), m.group(2)))
o = subprocess.run(['arm-linux-gnueabihf-addr2line', '-a', '-i', '-f', '-e', binp, hex(start + 4)] +
                   [hex(a) for a, _ in rows], capture_output=True, text=True).stdout.splitlines()
chains = {}; cur = None; i = 0
while i < len(o):
    if o[i].startswith('0x'):
        cur = int(o[i], 16); chains[cur] = []; i += 1; continue
    chains[cur].append((o[i], o[i + 1])); i += 2
hist = defaultdict(float)
with open(addrf + '.outer', 'w') as f:
    for a, ins in rows:
        ch = chains.get(a, [])
        inner = ch[0][1].split('/')[-1] if ch else '?'
        key = '?'
        for fn, loc in ch:
            if outer in fn:
                key = loc.split('/')[-1].split(' ')[0]
        p = pct.get(a, 0.0)
        hist[key] += p
        f.write(f'{p:6.2f} {a:x} {key:18} {inner:22} {ins[:110]}\n')
print(f'# {sum(hist.values()):.2f}% in symbol')
for k, v in sorted(hist.items(), key=lambda x: -x[1])[:30]:
    print(f'{v:6.2f} {k}')
