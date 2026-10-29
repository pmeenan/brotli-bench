#!/usr/bin/env python3
"""Compact, diffable structural dump of a brotli stream.

Usage: brstruct.py STREAM.br ORIGINAL_INPUT [--lengths] [--no-cmds]

Needs the original (decoded) input to compute literal contexts (we never
reconstruct copies / dictionary words ourselves). Prints one line per element
(meta-block header fields, context maps, prefix-code hashes, block switches,
commands) without bit offsets, so `diff` of the C vs Rust dumps shows the first
structural divergence. `--lengths` prints full code-length vectors.
"""
import hashlib
import os
import re
import sys

# Project root (this script lives in `scripts/`).
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_ctx_lut():
    src = open(os.path.join(REPO, 'vendor/brotli-c/c/common/context.c')).read()
    body = src[src.index('{', src.index('_kBrotliContextLookupTable')) + 1:]
    body = body[:body.index('};')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    vals = [int(x) for x in re.findall(r'\d+', body)]
    assert len(vals) == 2048, len(vals)
    return vals


LUT = load_ctx_lut()


def load_transforms():
    src = open(os.path.join(REPO, 'vendor/brotli-c/c/common/transform.c')).read()
    a = src.index('kPrefixSuffix[217] =')
    b = src.index(';', a)
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', re.sub(r'/\*.*?\*/', '', src[a:b], flags=re.S))
    raw = ''.join(lits).encode('latin-1').decode('unicode_escape').encode('latin-1') + b'\0'
    a = src.index('kPrefixSuffixMap[50]')
    pmap = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', src[a:src.index('};', a)])]
    a = src.index('kTransformsData[]')
    body = src[src.index('{', a) + 1:src.index('};', a)]
    hdr = open(os.path.join(REPO, 'vendor/brotli-c/c/common/transform.h')).read()
    enum = dict((k, int(v)) for k, v in re.findall(r'(BROTLI_TRANSFORM_\w+) = (\d+)', hdr))
    toks = [enum[t] if t in enum else int(t) for t in re.findall(r'BROTLI_TRANSFORM_\w+|\d+', body)]
    trs = []
    for i in range(0, len(toks), 3):
        pl = raw[pmap[toks[i]]]
        sl = raw[pmap[toks[i + 2]]]
        t = toks[i + 1]
        omit = t if 1 <= t <= 9 else (t - 11 if 12 <= t <= 20 else 0)
        trs.append((pl, omit, sl))
    assert len(trs) == 121, len(trs)
    return trs


TRANSFORMS = load_transforms()
SIZE_BITS = [0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8,
             7, 7, 6, 6, 5, 5]


class Bits:
    def __init__(self, data):
        self.data = data
        self.pos = 0  # bit position

    def read(self, n):
        v = 0
        for i in range(n):
            byte = self.data[self.pos >> 3]
            v |= ((byte >> (self.pos & 7)) & 1) << i
            self.pos += 1
        return v

    def peek(self, n):
        p = self.pos
        v = self.read(n)
        self.pos = p
        return v

    def align(self):
        self.pos = (self.pos + 7) & ~7


class Huff:
    def __init__(self, lengths):
        self.lengths = lengths
        nz = [(l, s) for s, l in enumerate(lengths) if l]
        self.single = None
        if len(nz) <= 1:
            self.single = nz[0][1] if nz else 0
            return
        nz.sort()
        code = 0
        prevlen = 0
        self.table = {}
        for l, s in nz:
            code <<= (l - prevlen)
            prevlen = l
            self.table[(l, code)] = s
            code += 1
        self.maxlen = prevlen

    def decode(self, br):
        if self.single is not None:
            return self.single
        code = 0
        for l in range(1, self.maxlen + 1):
            code = (code << 1) | br.read(1)
            s = self.table.get((l, code))
            if s is not None:
                return s
        raise ValueError('bad code')

    def digest(self):
        if self.single is not None:
            return 'sym%d' % self.single
        return hashlib.md5(bytes(self.lengths)).hexdigest()[:8]


CL_ORDER = [1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15]
CLP_LEN = [2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4]
CLP_VAL = [0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5]


def read_prefix_code(br, alphabet_size):
    hskip = br.read(2)
    lengths = [0] * alphabet_size
    if hskip == 1:
        nbits = max(1, (alphabet_size - 1).bit_length())
        nsym = br.read(2) + 1
        syms = [br.read(nbits) for _ in range(nsym)]
        if nsym == 1:
            h = Huff(lengths)
            h.single = syms[0]
            h.kind = 'simple1'
            return h
        if nsym == 2:
            ls = [1, 1]
        elif nsym == 3:
            ls = [1, 2, 2]
        else:
            ls = [1, 2, 3, 3] if br.read(1) else [2, 2, 2, 2]
        for s, l in zip(syms, ls):
            lengths[s] = l
        h = Huff(lengths)
        h.kind = 'simple%d' % nsym
        return h
    cl_lens = [0] * 18
    space = 32
    for i in range(hskip, 18):
        p = br.peek(4)
        br.read(CLP_LEN[p])
        v = CLP_VAL[p]
        cl_lens[CL_ORDER[i]] = v
        if v:
            space -= 32 >> v
            if space <= 0:
                break
    clh = Huff(cl_lens)
    symbol = 0
    prev = 8
    repeat = 0
    repeat_len = 0
    space = 32768
    while symbol < alphabet_size and space > 0:
        p = clh.decode(br)
        if p < 16:
            repeat = 0
            lengths[symbol] = p
            if p:
                prev = p
                space -= 32768 >> p
            symbol += 1
        else:
            extra = 2 if p == 16 else 3
            new_len = prev if p == 16 else 0
            if repeat_len != new_len:
                repeat = 0
                repeat_len = new_len
            old = repeat
            if repeat > 0:
                repeat = (repeat - 2) << extra
            repeat += br.read(extra) + 3
            delta = repeat - old
            for k in range(delta):
                lengths[symbol + k] = new_len
            if new_len:
                space -= delta * (32768 >> new_len)
            symbol += delta
    h = Huff(lengths)
    h.kind = 'complex%d' % hskip
    return h


def read_varlen8(br):
    if br.read(1) == 0:
        return 1
    n = br.read(3)
    if n == 0:
        return 2
    return (1 << n) + br.read(n) + 1


BL_OFF = [1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209,
          241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625]
BL_BITS = [2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10,
           11, 12, 13, 24]


def read_block_len(br, h):
    s = h.decode(br)
    return BL_OFF[s] + br.read(BL_BITS[s])


def read_context_map(br, size, ntrees):
    rlemax = 0
    if br.read(1):
        rlemax = br.read(4) + 1
    h = read_prefix_code(br, ntrees + rlemax)
    cmap = []
    while len(cmap) < size:
        s = h.decode(br)
        if s == 0:
            cmap.append(0)
        elif s <= rlemax:
            cmap.extend([0] * ((1 << s) + br.read(s)))
        else:
            cmap.append(s - rlemax)
    imtf = br.read(1)
    if imtf:
        mtf = list(range(256))
        for i, v in enumerate(cmap):
            val = mtf[v]
            cmap[i] = val
            if v:
                mtf.pop(v)
                mtf.insert(0, val)
    return cmap, rlemax, imtf


INS_BASE = [0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194,
            322, 578, 1090, 2114, 6210, 22594]
INS_EXTRA = [0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10,
             12, 14, 24]
CPY_BASE = [2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102,
            134, 198, 326, 582, 1094, 2118]
CPY_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9,
             10, 24]
CELLS = [(0, 0), (0, 8), (0, 0), (0, 8), (8, 0), (8, 8), (0, 16), (16, 0),
         (8, 16), (16, 8), (16, 16)]


class BlockState:
    def __init__(self, br, name, out):
        self.n = read_varlen8(br)
        self.name = name
        self.type = 0
        self.last = [1, 0]  # [second last, last]
        self.left = 1 << 30
        if self.n >= 2:
            self.th = read_prefix_code(br, self.n + 2)
            self.lh = read_prefix_code(br, 26)
            self.left = read_block_len(br, self.lh)
            out.append('  BT%s n=%d typecode=%s/%s lencode=%s/%s first_len=%d' % (
                name, self.n, self.th.kind, self.th.digest(), self.lh.kind,
                self.lh.digest(), self.left))
        else:
            out.append('  BT%s n=1' % name)

    def step(self, br, out, pos):
        if self.left == 0:
            s = self.th.decode(br)
            if s == 0:
                t = self.last[0]
            elif s == 1:
                t = (self.last[1] + 1) % self.n
            else:
                t = s - 2
            self.last = [self.last[1], t]
            self.type = t
            self.left = read_block_len(br, self.lh)
            out.append('    SW%s pos=%d type=%d len=%d' % (self.name, pos, t, self.left))
        self.left -= 1


def dump(path, orig, show_lengths=False, show_cmds=True):
    data = open(path, 'rb').read()
    src = open(orig, 'rb').read()
    br = Bits(data)
    out = []
    if br.read(1) == 0:
        wbits = 16
    else:
        n = br.read(3)
        if n:
            wbits = 17 + n
        else:
            m = br.read(3)
            wbits = 8 + m if m else 17
    out.append('WBITS %d' % wbits)
    maxdist = (1 << wbits) - 16
    total = 0
    rb = [16, 15, 11, 4]  # rb[-1] is last distance
    mb = 0
    while True:
        islast = br.read(1)
        if islast and br.read(1):
            out.append('MB%d last-empty' % mb)
            break
        mn = br.read(2)
        if mn == 3:
            assert br.read(1) == 0
            nb = br.read(2)
            skip = br.read(8 * nb) + 1 if nb else 0
            br.align()
            br.pos += 8 * skip
            out.append('MB%d metadata skip=%d' % (mb, skip))
            mb += 1
            if islast:
                break
            continue
        mlen = br.read((mn + 4) * 4) + 1
        unc = 0 if islast else br.read(1)
        out.append('MB%d start=%d mlen=%d last=%d uncompressed=%d' % (mb, total, mlen, islast, unc))
        if unc:
            br.align()
            br.pos += 8 * mlen
            total += mlen
            mb += 1
            continue
        L = BlockState(br, 'L', out)
        I = BlockState(br, 'I', out)
        D = BlockState(br, 'D', out)
        npostfix = br.read(2)
        ndirect = br.read(4) << npostfix
        out.append('  NPOSTFIX=%d NDIRECT=%d' % (npostfix, ndirect))
        modes = [br.read(2) for _ in range(L.n)]
        out.append('  CMODES %s' % ''.join('LMUS'[m] for m in modes))
        ntl = read_varlen8(br)
        if ntl >= 2:
            cmapl, rle, imtf = read_context_map(br, 64 * L.n, ntl)
            out.append('  NTREESL=%d rlemax=%d imtf=%d' % (ntl, rle, imtf))
            for t in range(L.n):
                out.append('   CMAPL[%d] %s' % (t, ' '.join(map(str, cmapl[64 * t:64 * t + 64]))))
        else:
            cmapl = [0] * (64 * L.n)
            out.append('  NTREESL=1')
        ntd = read_varlen8(br)
        if ntd >= 2:
            cmapd, rle, imtf = read_context_map(br, 4 * D.n, ntd)
            out.append('  NTREESD=%d rlemax=%d imtf=%d CMAPD %s' % (
                ntd, rle, imtf, ' '.join(map(str, cmapd))))
        else:
            cmapd = [0] * (4 * D.n)
            out.append('  NTREESD=1')
        dist_alpha = 16 + ndirect + (48 << npostfix)
        hl, hi, hd = [], [], []
        for name, lst, cnt, alpha in (('HL', hl, ntl, 256), ('HI', hi, I.n, 704),
                                      ('HD', hd, ntd, dist_alpha)):
            for i in range(cnt):
                h = read_prefix_code(br, alpha)
                lst.append(h)
                out.append('  %s[%d] %s %s' % (name, i, h.kind, h.digest()))
                if show_lengths:
                    out.append('    ' + ' '.join('%d:%d' % (s, l) for s, l in enumerate(h.lengths) if l))
        pos = 0
        ncmd = 0
        while pos < mlen:
            I.step(br, out, total + pos)
            sym = hi[I.type].decode(br)
            cell = CELLS[sym >> 6]
            ic = cell[0] + ((sym >> 3) & 7)
            cc = cell[1] + (sym & 7)
            ins = INS_BASE[ic] + br.read(INS_EXTRA[ic])
            cpy = CPY_BASE[cc] + br.read(CPY_EXTRA[cc])
            for k in range(ins):
                L.step(br, out, total + pos)
                ap = total + pos
                p1 = src[ap - 1] if ap >= 1 else 0
                p2 = src[ap - 2] if ap >= 2 else 0
                m = modes[L.type]
                ctx = LUT[m * 512 + p1] | LUT[m * 512 + 256 + p2]
                lit = hl[cmapl[64 * L.type + ctx]].decode(br)
                if lit != src[ap]:
                    sys.stderr.write('\n'.join(out[-40:]) + '\n'); raise ValueError('literal mismatch at %d ctx %d mode %d ltype %d got %d' % (ap, ctx, m, L.type, lit))
                pos += 1
            if pos >= mlen:
                if show_cmds:
                    out.append('   C%d @%d ins=%d cpy=%d (no copy, end of MB) sym=%d' % (
                        ncmd, total + pos - ins, ins, cpy, sym))
                ncmd += 1
                break
            if sym < 128:
                dcode = -1
                dist = rb[-1]
            else:
                D.step(br, out, total + pos)
                dctx = 3 if cpy > 4 else cpy - 2
                dcode = hd[cmapd[4 * D.type + dctx]].decode(br)
                if dcode < 16:
                    if dcode < 4:
                        dist = rb[-1 - dcode]
                    else:
                        base = rb[-1] if dcode < 10 else rb[-2]
                        k = (dcode - 4) % 6
                        dist = base + [-1, 1, -2, 2, -3, 3][k]
                elif dcode < 16 + ndirect:
                    dist = dcode - 15
                else:
                    d = dcode - ndirect - 16
                    postfix = d & ((1 << npostfix) - 1)
                    d >>= npostfix
                    hcode = d >> 1
                    lcode = d & 1
                    nbits = 1 + hcode
                    offset = ((2 + lcode) << nbits) - 4
                    dist = ((offset + br.read(nbits)) << npostfix) + postfix + ndirect + 1
            md = min(total + pos, maxdist)
            dictref = dist > md
            if not dictref and dcode > 0:
                rb.append(dist)
                rb.pop(0)
            if show_cmds:
                out.append('   C%d @%d ins=%d cpy=%d dist=%d dcode=%d%s' % (
                    ncmd, total + pos - ins, ins, cpy, dist, dcode, ' DICT' if dictref else ''))
            ncmd += 1
            if dictref:
                addr = dist - md - 1
                tidx = addr >> SIZE_BITS[cpy]
                pl, omit, sl = TRANSFORMS[tidx]
                pos += pl + max(0, cpy - omit) + sl
            else:
                pos += cpy
        out.append('  MB%d commands=%d' % (mb, ncmd))
        total += mlen
        mb += 1
        if islast:
            break
    assert total == len(src), (total, len(src))
    return out


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    lines = dump(args[0], args[1], '--lengths' in sys.argv, '--no-cmds' not in sys.argv)
    sys.stdout.write('\n'.join(lines) + '\n')
