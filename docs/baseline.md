# Baseline: Rust vs C brotli (2026-10-29)

This is the reference measurement taken before any optimization work. Compare
future runs against it with
`scripts/summarize_bench.py NEW.csv --baseline docs/baseline/bench-2026-10-29.csv`.

| Artifact | What |
| --- | --- |
| [baseline/bench-2026-10-29.csv](baseline/bench-2026-10-29.csv) | Raw bench results: 25 cases x 2 modes x q0-11 = 600 cells (run 2, the reference) |
| [baseline/bench-2026-10-29-run1.csv](baseline/bench-2026-10-29-run1.csv) | An earlier identical run, kept to quantify run-to-run drift |
| [baseline/bench-2026-10-29.txt](baseline/bench-2026-10-29.txt) | Console output of the same run |
| [baseline/bench-2026-10-29-details.md](baseline/bench-2026-10-29-details.md) | Per-case tables (generated) |
| [baseline/verify-2026-10-29.csv](baseline/verify-2026-10-29.csv) | `--verify` results: sizes, bit-exactness, first differing byte |
| [baseline/environment-2026-10-29.md](baseline/environment-2026-10-29.md) | Machine, toolchains, submodule SHAs |

Output differences between the encoders are analyzed in
[encoder-parity.md](encoder-parity.md).

## Setup

- **Machine:** GCE VM, 2x 24-core Intel Xeon @ 2.00GHz (Skylake-SP, family 6
  model 85, AVX-512), 96 vCPUs, Linux 7.1. No control over frequency scaling.
  The bench was pinned to CPU 2 with its SMT sibling (CPU 50) left idle, on
  an otherwise idle machine. Run 1 had light, single-threaded experiments
  running on the other socket.
- **Code:** brotli-c `d89c4b1` (v1.2.0+195), rust-brotli `6df909c` (9.1.1 /
  brotli-ffi 3.1.0), rust-brotli-decompressor `2ed02ed` (6.2.0).
- **Build:** C with gcc 15.2 `-O3 -march=native`. Rust with the Chromium
  nightly rustc 1.101, `-C target-cpu=native`, opt-level 3, fat LTO, 1 codegen
  unit, `panic=abort`.
- **Command:**
  ```bash
  taskset -c 2 ./build/brotli_compare --bench --set all \
    --corpus "alice29.txt,lcet10.txt,asyoulik.txt,monkey,.css,.js,.html,dict:" \
    --bench-csv docs/baseline/bench-2026-10-29.csv
  ```
  Each run took 19 minutes. All 600 cells passed the per-run checks: every encode
  succeeded with a deterministic size, every decode succeeded with the right
  size, and each cell's decoded output was fully verified once.
- **Method** (see `AGENTS.md` > Run): streaming API with a 32KB output buffer
  and lgwin 22. Static mode feeds the whole input with PROCESS + FINISH;
  flushed mode feeds 4KB input chunks with FLUSH after each. Decoding reads
  static streams in 16KB chunks and flushed streams one wire chunk at a time.
  Decode always uses the **C-encoded** stream for both decoders. Timings
  include instance create/destroy and dictionary attach/prepare. Each cell does
  a warmup, then alternates C and Rust runs for at least 3 iterations and
  300 ms. Reported values are median MiB/s of *uncompressed* data.
- **Noise within a run:** the median spread (half the interquartile range)
  was 1.0% for C encode, 1.2% for Rust encode, and 1.0% for decode on both
  sides. The p90 spread was 2.6% (C encode), 6.6% (Rust encode) and
  1.4-1.6% (decode).
- **Run-to-run drift (run 2 vs run 1):**
  - **Decode is very stable:** 5th-95th percentile cell ratio is 0.99-1.01,
    and the geomean changed by under 0.3%.
  - **Encode is mostly within +/-5%,** but q8-q10 are not. At q9 *both* C and
    Rust encode were up to 2.5x faster in run 2, and q8 C and q10 Rust moved
    10-30%.
  - **Why:** these qualities allocate tens of MB of hash tables per stream,
    so they are dominated by page faults and transparent-huge-page behavior.
    That depends on kernel memory state, not on the code.
  - **What to do:** for encode changes at q8-q10, compare against a
    back-to-back rerun of the unmodified build on the same core rather than
    against this CSV. Rust/C *ratios* within a run are more robust, because C
    and Rust are interleaved.

## Headline numbers

In the tables below, "standard" is the 17 non-dictionary cases and
"dictionary" is the 8 raw-dictionary cases. Each cell is the geometric mean of
the Rust/C ratio over the cases. Negative means Rust is slower or smaller.

#### Decode: Rust vs C throughput (geomean)

Both decoders decode the same C-encoded stream. Negative = Rust slower.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | -14.0% | -13.5% | -17.9% | -16.9% |
| 1 | -15.2% | -14.8% | -16.0% | -15.4% |
| 2 | -15.4% | -13.0% | -15.5% | -12.8% |
| 3 | -13.5% | -12.6% | -12.8% | -13.0% |
| 4 | -12.1% | -12.5% | -13.8% | -14.0% |
| 5 | -10.6% | -12.0% | -12.9% | -14.5% |
| 6 | -10.7% | -12.1% | -12.8% | -14.6% |
| 7 | -10.2% | -12.1% | -12.8% | -14.7% |
| 8 | -10.5% | -12.1% | -12.9% | -14.7% |
| 9 | -10.5% | -12.1% | -12.7% | -14.6% |
| 10 | -7.5% | -14.0% | -10.2% | -16.5% |
| 11 | -8.8% | -14.1% | -11.2% | -16.3% |
| **all** | -11.6% | -12.9% | -13.5% | -14.8% |

#### Encode: Rust vs C throughput (geomean)

Each encoder produces its own stream (see sizes). Negative = Rust slower.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | -40.0% | +129.4% | -26.2% | -6.4% |
| 1 | -46.2% | +72.9% | -38.9% | -16.3% |
| 2 | -37.5% | +72.6% | -32.1% | +77.7% |
| 3 | -35.9% | +85.3% | -28.6% | +67.8% |
| 4 | -26.5% | +80.6% | -20.1% | +61.2% |
| 5 | -31.5% | +41.4% | -27.4% | +34.4% |
| 6 | -35.7% | +21.0% | -31.9% | +21.1% |
| 7 | -45.4% | -0.7% | -42.3% | +1.8% |
| 8 | -55.8% | -25.7% | -51.5% | -21.7% |
| 9 | -19.1% | -0.9% | -20.6% | +1.3% |
| 10 | -25.3% | -27.7% | -28.2% | -19.5% |
| 11 | -6.9% | -16.2% | -15.5% | -10.2% |
| **all** | -35.0% | +26.9% | -31.0% | +11.2% |

#### Compressed size (Rust vs C, geomean) and bit-exact cells

Positive = Rust output larger. `exact` = cells where the Rust stream is byte-identical to C.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | +0.0% (17/17 exact) | -0.0% (0/8 exact) | +0.0% (17/17 exact) | +0.0% (0/8 exact) |
| 1 | +0.0% (17/17 exact) | +0.0% (0/8 exact) | +0.0% (17/17 exact) | +0.0% (0/8 exact) |
| 2 | -0.0% (1/17 exact) | -67.6% (0/8 exact) | -0.0% (1/17 exact) | -60.8% (0/8 exact) |
| 3 | +0.0% (1/17 exact) | +42.1% (0/8 exact) | -0.0% (1/17 exact) | +30.4% (0/8 exact) |
| 4 | +0.0% (1/17 exact) | +32.5% (0/8 exact) | +0.0% (1/17 exact) | +21.5% (0/8 exact) |
| 5 | +0.0% (14/17 exact) | +11.6% (0/8 exact) | +0.0% (14/17 exact) | +6.0% (0/8 exact) |
| 6 | +0.0% (15/17 exact) | +5.0% (0/8 exact) | +0.0% (15/17 exact) | +3.0% (0/8 exact) |
| 7 | +0.0% (15/17 exact) | +0.9% (0/8 exact) | +0.0% (15/17 exact) | +0.6% (0/8 exact) |
| 8 | +0.0% (15/17 exact) | -0.6% (0/8 exact) | +0.0% (15/17 exact) | -0.1% (0/8 exact) |
| 9 | +0.0% (2/17 exact) | -1.0% (0/8 exact) | -0.0% (2/17 exact) | -1.3% (0/8 exact) |
| 10 | +0.1% (1/17 exact) | +0.0% (0/8 exact) | +0.0% (1/17 exact) | +0.9% (0/8 exact) |
| 11 | -0.0% (1/17 exact) | +0.3% (0/8 exact) | +0.0% (1/17 exact) | +0.9% (0/8 exact) |

Typical absolute throughput: medians over the 16 standard cases, excluding
the tiny `monkey`. Encode and decode are in MiB/s for static mode. The last
column is the median per-chunk decode latency in flushed mode.

| Q | C enc | Rust enc | C dec | Rust dec | Flushed chunk latency C / Rust (us) |
|---:|---:|---:|---:|---:|---:|
| 0 | 200.7 | 116.9 | 209.0 | 183.5 | 24.3 / 29.6 |
| 1 | 161.9 | 85.7 | 219.5 | 188.3 | 25.7 / 30.5 |
| 2 | 69.4 | 45.1 | 232.8 | 206.3 | 21.1 / 23.8 |
| 3 | 62.5 | 42.0 | 244.0 | 215.2 | 20.3 / 22.8 |
| 4 | 40.4 | 32.3 | 261.2 | 233.9 | 19.6 / 22.4 |
| 5 | 26.9 | 20.6 | 278.1 | 250.8 | 18.6 / 21.1 |
| 6 | 24.1 | 17.8 | 287.5 | 258.6 | 18.1 / 20.6 |
| 7 | 20.0 | 12.8 | 284.9 | 262.4 | 17.8 / 20.2 |
| 8 | 18.9 | 9.4 | 295.8 | 265.3 | 17.6 / 20.1 |
| 9 | 8.8 | 6.7 | 296.4 | 265.8 | 17.6 / 20.1 |
| 10 | 1.3 | 1.1 | 216.1 | 208.7 | 23.1 / 25.1 |
| 11 | 0.4 | 0.4 | 246.8 | 233.7 | 21.4 / 23.4 |

### Fixed per-stream cost (1-byte input)

Measured separately with `bench_c` / `bench_rust` on a 1-byte file, pinned to
CPU 4. Each value is the median time per stream (create, attach, encode or
decode, destroy). Repeat runs agree within about 10%:

| Op | Q | C (us) | Rust (us) |
|---|---:|---:|---:|
| encode | 0 | 5.68 | 7.51 |
| encode | 1 | 0.76 | 0.84 |
| encode | 2 | 0.44 | 10.71 |
| encode | 3 | 0.44 | 10.52 |
| encode | 4 | 0.45 | 19.90 |
| encode | 5 | 0.54 | 77.06 |
| encode | 6 | 0.54 | 167.09 |
| encode | 7 | 0.50 | 666.40 |
| encode | 8 | 0.50 | 1376.30 |
| encode | 9 | 16.91 | 21.50 |
| encode | 10 | 20.20 | 7081.39 |
| encode | 11 | 22.25 | 7381.46 |
| decode | - | 0.50 | 0.69 |
| decode (64KB dictionary) | - | 0.56 | 0.73 |

## Observations

1. **Decoder: Rust is 10-15% slower almost everywhere.**
   - The gap is fairly uniform across content types and qualities: -11.6%
     geomean in static mode and -13.5% in flushed mode.
   - It is largest at q0-q2 (-14 to -18%), which are literal-heavy streams
     with simple prefix codes. It is smallest at q10/q11 (-7.5 to -11%) on
     the standard cases.
   - With a dictionary the gap is about 1.5 points worse overall, and up to
     6 points worse at q10/q11.
   - Cases dominated by setup are the worst: `monkey` (843 B) is -23 to -28%,
     and the near-identical bootstrap delta (193-byte stream) is -24 to -27%.
     That points at per-stream fixed costs in addition to the inner loop.
   - Flushed per-chunk decode latency is 2-5 us (about 10-20%) higher in Rust.
2. **Encoder, no dictionary: Rust is 7-56% slower depending on quality.**
   It is worst at q0/q1 (-26 to -46%, the fast fragment compressors) and at
   q7/q8 (-42 to -56%). It is best at q9 and q11 (-7 to -21%). Rust's output
   is the same size as C's at every quality (within 0.1%), so this is purely
   a speed gap.
3. **Rust has a large fixed setup cost per encoder instance.**
   - For a 1-byte input, Rust takes 10-20 us at q2-q4, 70-1350 us at q5-q8,
     and about 7 ms at q10/q11. C takes under 1 us (about 20 us at q9-q11).
   - C sizes and clears its hash tables lazily based on the input; Rust
     allocates and initializes full-size tables up front.
   - This dominates small responses and small flushed streams: `monkey`
     encode is -65 to -67%. It is likely the single biggest encoder win for
     typical web payloads.
4. **The dictionary encode numbers are not like-for-like.**
   - C re-runs `BrotliEncoderPrepareDictionary` on every stream. That builds
     a quality-independent index of about 2 MB, which costs about 1.3 ms for
     a 64KB dictionary even at q0-q2, where C never uses it.
   - Rust's legacy `BrotliEncoderSetCustomDictionary` path does no such work
     at q0/q1, and hashes the dictionary into its regular hasher at q2 and
     above. That is why Rust looks 73-129% faster at q0-q4 in static mode with
     a dictionary (61-78% at q2-q4 flushed).
   - At flushed q0/q1, Rust is 6-16% *slower*. Setting a dictionary forces it
     off its fast path (finding D1 in [encoder-parity.md](encoder-parity.md)).
   - The two encoders also use different dictionary models, so the sizes
     differ a lot: Rust is -61 to -68% at q2, +30 to +42% at q3, and within
     about 1.3% at q7-q11. See
     [encoder-parity/dictionary.md](encoder-parity/dictionary.md).
   - A fair comparison needs prepared-dictionary reuse on the C side and the
     prepared-dictionary API in Rust.
5. **Static vs flushed.** The relative gaps are similar in both modes. The
   Rust decoder is 1-3 points worse when flushed (more, smaller calls).

## Reproducing / comparing

```bash
# Full baseline-equivalent run (~20 min); keep the machine quiet and pinned.
taskset -c 2 ./build/brotli_compare --bench --set all \
  --corpus "alice29.txt,lcet10.txt,asyoulik.txt,monkey,.css,.js,.html,dict:" \
  --bench-csv /tmp/new.csv
python3 scripts/summarize_bench.py /tmp/new.csv --baseline docs/baseline/bench-2026-10-29.csv

# Quick check on the default subset (6 cases)
taskset -c 2 ./build/brotli_compare --bench --bench-csv /tmp/quick.csv
python3 scripts/summarize_bench.py /tmp/quick.csv --baseline docs/baseline/bench-2026-10-29.csv
```

The C-vs-baseline table printed by `--baseline` shows run-to-run drift, since
the C code doesn't change between runs. Treat Rust changes smaller than that
drift as noise.
