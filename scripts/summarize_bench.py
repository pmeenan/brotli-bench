#!/usr/bin/env python3
"""Summarizes `brotli_compare --bench --bench-csv` output as markdown.

Usage:
  scripts/summarize_bench.py BENCH.csv [--baseline OLD.csv] [--details]

Prints geometric-mean Rust/C throughput ratios per quality (encode and
decode, static and flushed, standard vs dictionary cases). With `--baseline`,
also prints how Rust (and C) throughput changed relative to an older CSV
from the same machine. `--details` adds a per-case table.
"""

import argparse
import csv
import math
import sys
from collections import defaultdict


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in ("quality", "input_size", "dict_size", "c_size", "rust_size",
                  "exact", "enc_iters", "dec_iters"):
            r[k] = int(r[k])
        for k in ("c_enc_mibs", "c_enc_spread_pct", "rust_enc_mibs",
                  "rust_enc_spread_pct", "c_dec_mibs", "c_dec_spread_pct",
                  "rust_dec_mibs", "rust_dec_spread_pct", "c_chunk_us",
                  "rust_chunk_us"):
            r[k] = float(r[k])
        r["kind"] = "dictionary" if r["case"].startswith("dict:") else "standard"
    return rows


def geomean(values):
    values = [v for v in values if v > 0]
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def pct(ratio):
    return "n/a" if math.isnan(ratio) else f"{(ratio - 1.0) * 100:+.1f}%"


def ratio_table(rows, title, num_key, den_key, note):
    """Geomean of `num_key / den_key` per quality x (mode, kind)."""
    groups = [(m, k) for m in ("static", "flushed")
              for k in ("standard", "dictionary")]
    groups = [g for g in groups
              if any(r["mode"] == g[0] and r["kind"] == g[1] for r in rows)]
    qualities = sorted({r["quality"] for r in rows})
    out = [f"### {title}", "", note, ""]
    out.append("| Q | " + " | ".join(f"{m} {k}" for m, k in groups) + " |")
    out.append("|---:|" + "---:|" * len(groups))
    for q in qualities + ["all"]:
        cells = []
        for m, k in groups:
            sel = [r for r in rows if r["mode"] == m and r["kind"] == k and
                   (q == "all" or r["quality"] == q)]
            cells.append(pct(geomean(r[num_key] / r[den_key] for r in sel
                                     if r[den_key] > 0)))
        label = "**all**" if q == "all" else str(q)
        out.append(f"| {label} | " + " | ".join(cells) + " |")
    out.append("")
    return out


def size_table(rows):
    groups = [(m, k) for m in ("static", "flushed")
              for k in ("standard", "dictionary")]
    groups = [g for g in groups
              if any(r["mode"] == g[0] and r["kind"] == g[1] for r in rows)]
    qualities = sorted({r["quality"] for r in rows})
    out = ["### Compressed size (Rust vs C, geomean) and bit-exact cells", "",
           "Positive = Rust output larger. `exact` = cells where the Rust "
           "stream is byte-identical to C.", ""]
    out.append("| Q | " + " | ".join(f"{m} {k}" for m, k in groups) + " |")
    out.append("|---:|" + "---:|" * len(groups))
    for q in qualities:
        cells = []
        for m, k in groups:
            sel = [r for r in rows if r["mode"] == m and r["kind"] == k and
                   r["quality"] == q]
            g = geomean(r["rust_size"] / r["c_size"] for r in sel)
            exact = sum(r["exact"] for r in sel)
            cells.append(f"{pct(g)} ({exact}/{len(sel)} exact)")
        out.append(f"| {q} | " + " | ".join(cells) + " |")
    out.append("")
    return out


def details(rows):
    out = ["### Per-case results", "",
           "MiB/s of uncompressed data (median). Delta = Rust vs C.", ""]
    by_case = defaultdict(list)
    for r in rows:
        by_case[r["case"]].append(r)
    for case, rs in by_case.items():
        r0 = rs[0]
        extra = f", dictionary {r0['dict_size']} B" if r0["dict_size"] else ""
        out.append(f"#### `{case}` ({r0['input_size']} B{extra})")
        out.append("")
        out.append("| Mode | Q | C size | Rust size | C enc | Rust enc | Enc | "
                   "C dec | Rust dec | Dec |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in rs:
            out.append(
                f"| {r['mode']} | {r['quality']} | {r['c_size']} | "
                f"{r['rust_size']}{'' if not r['exact'] else ' (=)'} | "
                f"{r['c_enc_mibs']:.1f} | {r['rust_enc_mibs']:.1f} | "
                f"{pct(r['rust_enc_mibs'] / r['c_enc_mibs'])} | "
                f"{r['c_dec_mibs']:.1f} | {r['rust_dec_mibs']:.1f} | "
                f"{pct(r['rust_dec_mibs'] / r['c_dec_mibs'])} |")
        out.append("")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--baseline", help="older bench CSV to compare against")
    ap.add_argument("--details", action="store_true",
                    help="include per-case tables")
    args = ap.parse_args()

    rows = load(args.csv)
    out = []
    out += ratio_table(rows, "Decode: Rust vs C throughput (geomean)",
                       "rust_dec_mibs", "c_dec_mibs",
                       "Both decoders decode the same C-encoded stream. "
                       "Negative = Rust slower.")
    out += ratio_table(rows, "Encode: Rust vs C throughput (geomean)",
                       "rust_enc_mibs", "c_enc_mibs",
                       "Each encoder produces its own stream (see sizes). "
                       "Negative = Rust slower.")
    out += size_table(rows)

    if args.baseline:
        base = {(r["case"], r["mode"], r["quality"]): r
                for r in load(args.baseline)}
        joined = []
        for r in rows:
            b = base.get((r["case"], r["mode"], r["quality"]))
            if not b:
                continue
            j = dict(r)
            for k in ("rust_dec_mibs", "rust_enc_mibs", "c_dec_mibs",
                      "c_enc_mibs"):
                j["base_" + k] = b[k]
            joined.append(j)
        if joined:
            out += ratio_table(joined, "Rust decode vs baseline (geomean)",
                               "rust_dec_mibs", "base_rust_dec_mibs",
                               f"Baseline: `{args.baseline}`. Positive = faster.")
            out += ratio_table(joined, "Rust encode vs baseline (geomean)",
                               "rust_enc_mibs", "base_rust_enc_mibs",
                               f"Baseline: `{args.baseline}`. Positive = faster.")
            out += ratio_table(joined, "C decode vs baseline (noise check)",
                               "c_dec_mibs", "base_c_dec_mibs",
                               "C is unchanged between runs, so this shows "
                               "run-to-run drift.")
        else:
            print("warning: no overlapping cells with baseline", file=sys.stderr)

    if args.details:
        out += details(rows)
    print("\n".join(out))


if __name__ == "__main__":
    main()
