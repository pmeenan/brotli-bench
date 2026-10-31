#!/usr/bin/env python3
# Usage: annotate.py BINARY ADDRFILE SYMBOL_SUBSTR [min_pct]
# Prints objdump disassembly (with source lines) of the symbol, annotated with
# per-address sample percentages from `simpleperf report --sort vaddr_in_file`.
import re, subprocess, sys
binp, addrf, sym = sys.argv[1:4]
minp = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
pct = {}
for line in open(addrf):
    m = re.match(r'\s*([\d.]+)%\s+0x([0-9a-f]+)', line)
    if m:
        pct[int(m.group(2), 16)] = float(m.group(1))
nm = subprocess.run(['arm-linux-gnueabihf-nm', '-S', '--defined-only', binp], capture_output=True, text=True).stdout
cands = []
for l in nm.splitlines():
    p = l.split()
    if len(p) == 4 and sym in p[3]:
        cands.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
cands.sort(key=lambda c: -c[1])
start, size, name = cands[0]
print(f'# {name} {start:#x} size {size}')
tot = sum(v for a, v in pct.items() if start <= a < start + size)
print(f'# total in symbol: {tot:.2f}%')
out = subprocess.run(['arm-linux-gnueabihf-objdump', '-d', '-l', '--no-show-raw-insn',
                      f'--start-address={start:#x}', f'--stop-address={start+size:#x}', binp],
                     capture_output=True, text=True).stdout
src = ''
for l in out.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
    if m:
        a = int(m.group(1), 16)
        p = pct.get(a, 0.0)
        if p >= minp or minp == 0:
            print(f'{p:6.2f} {a:x}: {m.group(2)}   [{src}]')
    elif l.startswith('/') or 'rs:' in l or '.h:' in l or '.c:' in l:
        src = l.strip().split('/')[-1]
