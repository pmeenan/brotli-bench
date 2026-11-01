# Baseline: Rust vs C brotli (2026-10-29)

Two pre-optimization reference measurements:

- **[Perf host `linux`, 2026-10-30](#perf-host-baseline-linux-2026-10-30):**
  the reference for all optimization work from now on. It is a physical
  machine with a tuned, frequency-locked session; compare with
  `scripts/summarize_bench.py NEW.csv --baseline docs/baseline/bench-2026-10-30-linux.csv`.
- **GCE dev VM, 2026-10-29** (the sections directly below): the first
  baseline, plus the encoder-parity analysis. Kept for history; don't compare
  new perf-host numbers against it.

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

---

## Perf host baseline: `linux` (2026-10-30)

**This is the reference for all decoder optimization work.** All future
performance numbers come from this host
([perf-host.md](perf-host.md)). Compare against it with
`scripts/summarize_bench.py NEW.csv --baseline docs/baseline/bench-2026-10-30-linux.csv`.
Never compare numbers across hosts.

| Artifact | What |
| --- | --- |
| [baseline/bench-2026-10-30-linux.csv](baseline/bench-2026-10-30-linux.csv) | Raw bench results, 600 cells (same case selection as above) |
| [baseline/bench-2026-10-30-linux.txt](baseline/bench-2026-10-30-linux.txt) | Console output |
| [baseline/bench-2026-10-30-linux-details.md](baseline/bench-2026-10-30-linux-details.md) | Per-case tables (generated) |
| [baseline/verify-2026-10-30-linux.csv](baseline/verify-2026-10-30-linux.csv) | `--verify` on the host: 600/600 decode parity, 440/600 bit-exact |
| [baseline/environment-2026-10-30-linux.md](baseline/environment-2026-10-30-linux.md) | Commits, toolchain, machine, and host state before and after the session |
| [baseline/tune-report-2026-10-30-linux.json](baseline/tune-report-2026-10-30-linux.json) | `tune_benchmark_host.py` report: sysfs before, during and restored, plus observations |

### Setup

- **Machine:** Intel Core i9-11900KF (Rocket Lake), Ubuntu kernel 7.0.
- **Session:**
  - `hostlock exclusive` + `tune_benchmark_host.py run --keep-aslr`;
  - turbo off, `performance` governor and EPP, min = max = **3.5 GHz**;
  - NMI watchdog off, SMT off (CPUs 0-7 online), ASLR on;
  - `taskset -c 2`.
  - The frequency stayed at 3.5 GHz and the throttle counters stayed at 0
    throughout. Load average was about 1.0 (the session itself), and the
    tuner restored every setting afterwards.
- **Code:**
  - brotli-c `d89c4b1`.
  - rust-brotli **`27a1859` (`perf` = `fix-dict-span` + `parity`)**. This is
    not the unpatched `6df909c` used on GCE above, so encoder numbers here
    include the parity patches' costs.
  - rust-brotli-decompressor `2ed02ed` (unchanged).
- **Toolchain:**
  - gcc 15.2 `-O3 -march=native`;
  - the host's **stable** rustc 1.99.1, `-C target-cpu=native`, fat LTO,
    `panic=abort`;
  - cmake 4.2.3.
- **Command:** the full run took 17 minutes, followed by the 1-byte
  fixed-cost table in the same session:
  ```bash
  hostlock exclusive --label "brotli-bench: linux baseline" -- \
    python3 ~/src/ai-skills/optimize-campaign/scripts/tune_benchmark_host.py run --keep-aslr --report tune-report.json -- \
      bash run_baseline_inner.sh   # taskset -c 2 brotli_compare --bench --set all --corpus "<as above>" ...
  ```
- **Noise:**
  - Median half-IQR spread: 0.5% (C encode), 0.6% (Rust encode), 1.5-1.6%
    (decode).
  - p90 spread: 1.6% / 1.9% (encode), 2.3-2.5% (decode).
  - Encode is much steadier than on GCE.

### Headline numbers (linux)

#### Decode: Rust vs C throughput (geomean)

Both decoders decode the same C-encoded stream. Negative = Rust slower.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | -6.2% | -6.1% | -6.9% | -6.3% |
| 1 | -7.6% | -7.9% | -6.9% | -6.9% |
| 2 | -7.5% | -6.8% | -7.2% | -6.4% |
| 3 | -6.1% | -8.7% | -5.9% | -9.5% |
| 4 | -5.0% | -9.2% | -7.1% | -11.1% |
| 5 | -4.1% | -9.5% | -6.5% | -11.0% |
| 6 | -4.1% | -9.4% | -6.6% | -11.1% |
| 7 | -4.3% | -9.4% | -6.6% | -11.1% |
| 8 | -4.3% | -9.6% | -6.6% | -11.1% |
| 9 | -4.4% | -9.2% | -6.6% | -11.0% |
| 10 | -0.5% | -8.5% | -3.3% | -11.0% |
| 11 | -1.1% | -7.8% | -4.3% | -10.5% |
| **all** | -4.6% | -8.5% | -6.2% | -9.8% |

#### Encode: Rust vs C throughput (geomean)

Each encoder produces its own stream (see sizes). Negative = Rust slower.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | -33.2% | +145.5% | -19.7% | +77.4% |
| 1 | -36.2% | +93.7% | -29.5% | +55.9% |
| 2 | -32.3% | +94.6% | -26.7% | +96.2% |
| 3 | -25.9% | +109.9% | -18.2% | +85.4% |
| 4 | -17.7% | +98.6% | -12.6% | +71.5% |
| 5 | -24.1% | +53.2% | -21.0% | +41.9% |
| 6 | -28.8% | +39.2% | -25.7% | +31.8% |
| 7 | -37.5% | +13.3% | -34.8% | +10.8% |
| 8 | -44.6% | -4.7% | -41.7% | -4.6% |
| 9 | -37.7% | -19.8% | -36.9% | -18.5% |
| 10 | -40.0% | -36.1% | -38.8% | -28.7% |
| 11 | -25.8% | -25.9% | -26.2% | -19.3% |
| **all** | -32.4% | +34.1% | -28.2% | +26.0% |

#### Compressed size (Rust vs C, geomean) and bit-exact cells

Positive = Rust output larger. `exact` = cells where the Rust stream is byte-identical to C.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | +0.0% (17/17 exact) | +0.0% (8/8 exact) | +0.0% (17/17 exact) | +0.0% (8/8 exact) |
| 1 | +0.0% (17/17 exact) | +0.0% (8/8 exact) | +0.0% (17/17 exact) | +0.0% (8/8 exact) |
| 2 | +0.0% (17/17 exact) | -67.5% (0/8 exact) | +0.0% (17/17 exact) | -60.7% (0/8 exact) |
| 3 | +0.0% (17/17 exact) | +41.3% (0/8 exact) | +0.0% (17/17 exact) | +28.9% (0/8 exact) |
| 4 | +0.0% (17/17 exact) | +28.8% (0/8 exact) | +0.0% (17/17 exact) | +21.5% (0/8 exact) |
| 5 | +0.0% (17/17 exact) | +11.6% (0/8 exact) | +0.0% (17/17 exact) | +6.0% (0/8 exact) |
| 6 | +0.0% (17/17 exact) | +5.0% (0/8 exact) | +0.0% (17/17 exact) | +3.0% (0/8 exact) |
| 7 | +0.0% (17/17 exact) | +0.9% (0/8 exact) | +0.0% (17/17 exact) | +0.6% (0/8 exact) |
| 8 | +0.0% (17/17 exact) | -0.6% (0/8 exact) | +0.0% (17/17 exact) | -0.1% (0/8 exact) |
| 9 | +0.0% (17/17 exact) | -1.0% (0/8 exact) | +0.0% (17/17 exact) | -1.3% (0/8 exact) |
| 10 | +0.0% (17/17 exact) | -0.1% (0/8 exact) | +0.0% (17/17 exact) | +0.8% (0/8 exact) |
| 11 | +0.0% (17/17 exact) | +0.2% (0/8 exact) | +0.0% (17/17 exact) | +0.9% (0/8 exact) |

Typical absolute throughput at 3.5 GHz (medians over the 16 standard cases
excluding `monkey`; MiB/s, static mode; last column is the median flushed
per-chunk decode latency):

| Q | C enc | Rust enc | C dec | Rust dec | Flushed chunk latency C / Rust (us) |
|---:|---:|---:|---:|---:|---:|
| 0 | 293.3 | 192.2 | 320.4 | 300.1 | 16.3 / 17.3 |
| 1 | 224.3 | 140.4 | 333.6 | 307.9 | 16.9 / 18.1 |
| 2 | 105.4 | 77.0 | 348.6 | 337.6 | 13.8 / 14.6 |
| 3 | 89.5 | 70.1 | 365.0 | 354.1 | 13.4 / 14.1 |
| 4 | 57.7 | 50.5 | 391.1 | 388.0 | 13.1 / 13.7 |
| 5 | 41.3 | 34.0 | 418.5 | 419.6 | 12.4 / 12.9 |
| 6 | 37.2 | 29.7 | 432.5 | 433.9 | 12.1 / 12.6 |
| 7 | 31.4 | 22.5 | 438.1 | 439.0 | 11.9 / 12.4 |
| 8 | 29.4 | 18.8 | 445.7 | 446.0 | 11.7 / 12.3 |
| 9 | 10.4 | 7.3 | 446.8 | 446.2 | 11.7 / 12.3 |
| 10 | 2.0 | 1.4 | 320.1 | 327.1 | 15.6 / 15.8 |
| 11 | 0.7 | 0.6 | 364.4 | 374.2 | 14.4 / 14.7 |

Fixed per-stream cost (1-byte input, median per stream, same session):

| Op | Q | C (us) | Rust (us) |
|---|---:|---:|---:|
| encode | 0 | 3.50 | 4.66 |
| encode | 1 | 0.55 | 0.76 |
| encode | 2 | 0.30 | 4.69 |
| encode | 3 | 0.31 | 4.70 |
| encode | 4 | 0.30 | 10.93 |
| encode | 5 | 0.36 | 54.57 |
| encode | 6 | 0.35 | 112.89 |
| encode | 7 | 0.33 | 447.38 |
| encode | 8 | 0.33 | 890.78 |
| encode | 9 | 4.58 | 12890.76 |
| encode | 10 | 14.04 | 12914.72 |
| encode | 11 | 15.51 | 12888.30 |
| decode | - | 0.34 | 0.46 |
| decode (64KB dictionary) | - | 0.38 | 0.49 |

### Observations (linux vs GCE)

1. **The decoder gap depends on the microarchitecture.**
   - On Rocket Lake at 3.5 GHz, Rust decode is -4.6% (static) / -6.2%
     (flushed) on the standard cases, vs -11.6% / -13.5% on the Skylake-SP
     VM.
   - Per-quality *medians* are close to C at q4-q11 static, but the geomean
     is still negative.
   - q0-q2 remain the worst standard cells (-6 to -8%).
   - Decoder changes should be confirmed on more than one CPU before
     claiming parity. Chromium also ships arm64.
2. **Dictionary decode is now the largest relative gap:** -8.5% static /
   -9.8% flushed, about twice the standard-case gap. It is about -9 to -11%
   at q3-q11. This raises the priority of decoder workstream 6
   (dictionary decode) in [decoder-perf-plan.md](decoder-perf-plan.md).
3. **Fixed per-stream decode cost:** 0.34 us (C) vs 0.46 us (Rust), and
   0.38 vs 0.49 us with a dictionary. The ratio is the same as on GCE.
4. **Encoder, now on the parity branch:**
   - Output is bit-exact with C on all standard cells. Dictionary cells are
     exact at q0/q1; q2-q11 are still the D2/D3 model differences.
   - Speed is -32% static overall.
   - **New issue: q9 small-input setup.** The 1-byte q9 encode now costs
     about 12.9 ms (C: 4.6 us). Parity patch 0003 switched q9 to H5 with
     `block_bits=8`, whose large tables Rust allocates and clears up front,
     whereas C sizes them to the input. q9 encode throughput is -38% vs -19%
     for the unpatched encoder on GCE.
   - q10/q11 setup is likewise about 12.9 ms. That's slower than on GCE,
     likely page faults with THP set to `madvise` here.
   - Encoder work is tabled, but this belongs on the encoder perf list (see
     AGENTS.md known issues).

---

## Post-optimization decoder benchmark: `linux` (2026-10-30)

Measured after the decoder optimization series on
`vendor/rust-brotli-decompressor:perf` (`081c5d9`: commits `0466359`,
`1344c34`, `081c5d9`), compared against the pre-optimization `linux` baseline
above (`docs/baseline/bench-2026-10-30-linux.csv`, `2ed02ed`).

| Artifact | What |
| --- | --- |
| [baseline/bench-2026-10-30-decoder-perf-linux.csv](baseline/bench-2026-10-30-decoder-perf-linux.csv) | Raw bench results, 600 cells |
| [baseline/bench-2026-10-30-decoder-perf-linux.txt](baseline/bench-2026-10-30-decoder-perf-linux.txt) | Console output |
| [baseline/bench-2026-10-30-decoder-perf-linux-details.md](baseline/bench-2026-10-30-decoder-perf-linux-details.md) | Per-case tables (generated) |
| [baseline/verify-2026-10-30-decoder-perf-linux.csv](baseline/verify-2026-10-30-decoder-perf-linux.csv) | `--verify`: 600/600 decode parity, 440/600 bit-exact |
| [baseline/tune-report-2026-10-30-decoder-perf-linux.json](baseline/tune-report-2026-10-30-decoder-perf-linux.json) | `tune_benchmark_host.py` report |

### Setup

- **Machine & session:** identical to the `linux` baseline above (Intel Core
  i9-11900KF, `hostlock exclusive` + `tune_benchmark_host.py run --keep-aslr`,
  turbo off, `performance` governor, locked at 3.5 GHz, NMI watchdog off, SMT
  off, ASLR on, `taskset -c 2`). Zero thermal or power throttling events.
- **Code:**
  - brotli-c `d89c4b1` (unchanged);
  - rust-brotli `27a1859` (`perf`, unchanged);
  - rust-brotli-decompressor **`081c5d9` (`perf`)**.
- **Noise:**
  - Median half-IQR spread: 0.5% (C encode), 0.6% (Rust encode), 1.7% (C
    decode), 1.6% (Rust decode).
  - p90 spread: 1.6% / 1.9% (encode), 2.7% / 2.7% (decode).

### Headline numbers (linux, post-optimization)

#### Decode: Rust vs C throughput (geomean)

Both decoders decode the same C-encoded stream. Positive = Rust faster.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | +1.2% | +1.1% | +5.6% | +5.4% |
| 1 | +1.5% | +1.3% | +4.8% | +4.7% |
| 2 | +1.7% | +2.6% | +3.3% | +4.5% |
| 3 | +2.7% | +10.6% | +4.8% | +10.0% |
| 4 | +3.1% | +9.5% | +2.9% | +7.8% |
| 5 | +2.5% | +8.5% | +2.2% | +7.0% |
| 6 | +2.3% | +8.6% | +2.0% | +7.1% |
| 7 | +2.0% | +8.0% | +1.7% | +6.9% |
| 8 | +1.9% | +8.1% | +1.7% | +6.5% |
| 9 | +1.9% | +8.3% | +1.7% | +6.6% |
| 10 | +0.9% | +4.9% | +1.5% | +4.2% |
| 11 | -0.1% | +5.7% | +0.9% | +4.7% |
| **all** | **+1.8%** | **+6.4%** | **+2.7%** | **+6.3%** |

#### Rust decode vs baseline (geomean)

Baseline: `docs/baseline/bench-2026-10-30-linux.csv` (`2ed02ed`). Positive = faster.

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| 0 | +7.1% | +7.2% | +13.6% | +12.8% |
| 1 | +8.9% | +8.9% | +12.6% | +12.6% |
| 2 | +9.8% | +10.0% | +11.5% | +11.7% |
| 3 | +9.5% | +22.4% | +12.0% | +23.1% |
| 4 | +8.6% | +21.7% | +11.6% | +22.9% |
| 5 | +6.9% | +20.2% | +10.1% | +21.7% |
| 6 | +6.8% | +19.8% | +10.2% | +21.9% |
| 7 | +6.8% | +19.6% | +9.8% | +21.9% |
| 8 | +6.7% | +19.9% | +9.8% | +22.0% |
| 9 | +6.6% | +19.8% | +9.7% | +21.9% |
| 10 | +1.8% | +15.6% | +6.1% | +19.1% |
| 11 | +1.6% | +16.1% | +6.6% | +19.5% |
| **all** | **+6.7%** | **+16.7%** | **+10.3%** | **+19.2%** |

#### C decode vs baseline (noise check)

| Q | static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|---:|
| **all** | -0.0% | +0.3% | +0.6% | +1.2% |

Typical absolute throughput at 3.5 GHz (medians over the 16 standard cases
excluding `monkey`; MiB/s, static mode; last column is the median flushed
per-chunk decode latency):

| Q | C enc | Rust enc | C dec | Rust dec | Flushed chunk latency C / Rust (us) |
|---:|---:|---:|---:|---:|---:|
| 0 | 294.1 | 192.6 | 318.8 | 324.0 | 16.3 / 15.4 |
| 1 | 224.9 | 140.4 | 327.6 | 335.8 | 17.0 / 16.2 |
| 2 | 105.6 | 77.1 | 347.4 | 358.5 | 13.9 / 13.1 |
| 3 | 89.9 | 70.3 | 365.0 | 375.7 | 13.4 / 12.7 |
| 4 | 57.8 | 50.7 | 388.6 | 406.5 | 13.1 / 12.5 |
| 5 | 41.3 | 34.1 | 416.1 | 435.3 | 12.4 / 11.9 |
| 6 | 37.3 | 29.7 | 429.1 | 449.9 | 12.0 / 11.7 |
| 7 | 31.5 | 22.6 | 436.1 | 455.2 | 11.8 / 11.5 |
| 8 | 29.5 | 18.8 | 442.6 | 460.5 | 11.7 / 11.4 |
| 9 | 10.4 | 7.3 | 444.5 | 462.1 | 11.7 / 11.4 |
| 10 | 2.0 | 1.4 | 317.6 | 330.1 | 15.5 / 15.0 |
| 11 | 0.7 | 0.6 | 362.6 | 375.3 | 14.4 / 13.9 |

Fixed per-stream decode cost (1-byte input, 50,000 iterations, same session):

| Op | C (us) | Rust before (us) | Rust after (us) |
|---|---:|---:|---:|
| decode | 0.34 | 0.46 | 0.43 |
| decode (64KB dictionary) | 0.34 | 0.49 | 0.45 |

### Multi-platform, build-flag, and API comparison (2026-10-30)

Measured on the 144-cell default benchmark subset (`kDefaultBenchCases`:
`alice29.txt`, `jquery-3.7.1.min.js`, `bootstrap-5.3.3.min.css`,
`wiki-Brotli.html`, `dict:jquery-3.6.4-to-3.7.1.min.js`,
`dict:wiki-html-Brotli`, `q0..11`, `static` + `flushed`) across four platforms,
two x86-64 build configurations, and both Rust integration APIs (`--rust-api ffi`
calling `BrotliDecoderDecompressStream` vs `--rust-api direct` calling
`BrotliState<StandardAlloc, StandardAlloc, StandardAlloc>` +
`BrotliDecompressStream` directly):

| Artifact | Platform & Config |
| --- | --- |
| [baseline/bench-2026-10-30-linux-native-ffi.csv](baseline/bench-2026-10-30-linux-native-ffi.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`081c5d9`), C FFI |
| [baseline/bench-2026-10-30-linux-native-direct.csv](baseline/bench-2026-10-30-linux-native-direct.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`081c5d9`), Direct Rust API |
| [baseline/bench-2026-10-30-linux-native-ffi-post-opt.csv](baseline/bench-2026-10-30-linux-native-ffi-post-opt.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`05ee043`), C FFI |
| [baseline/bench-2026-10-30-linux-native-direct-post-opt.csv](baseline/bench-2026-10-30-linux-native-direct-post-opt.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`05ee043`), Direct Rust API |
| [baseline/bench-2026-10-30-linux-generic-base-ffi.csv](baseline/bench-2026-10-30-linux-generic-base-ffi.csv) | `linux` (i9-11900KF @ 3.5 GHz), generic `x86-64` + ThinLTO, baseline (`2ed02ed`), C FFI |
| [baseline/bench-2026-10-30-linux-generic-perf-ffi.csv](baseline/bench-2026-10-30-linux-generic-perf-ffi.csv) | `linux` (i9-11900KF @ 3.5 GHz), generic `x86-64` + ThinLTO, `perf` (`081c5d9`), C FFI |
| [baseline/bench-2026-10-30-linux-generic-perf-direct.csv](baseline/bench-2026-10-30-linux-generic-perf-direct.csv) | `linux` (i9-11900KF @ 3.5 GHz), generic `x86-64` + ThinLTO, `perf` (`081c5d9`), Direct Rust API |
| [baseline/bench-2026-10-30-linux-generic-ffi-post-opt.csv](baseline/bench-2026-10-30-linux-generic-ffi-post-opt.csv) | `linux` (i9-11900KF @ 3.5 GHz), generic `x86-64` + ThinLTO, `perf` (`05ee043`), C FFI |
| [baseline/bench-2026-10-30-linux-generic-post-opt.csv](baseline/bench-2026-10-30-linux-generic-post-opt.csv) | `linux` (i9-11900KF @ 3.5 GHz), generic `x86-64` + ThinLTO, `perf` (`05ee043`), Direct Rust API |
| [baseline/bench-2026-10-30-mac-base-ffi.csv](baseline/bench-2026-10-30-mac-base-ffi.csv) | `mac` (Apple M5 Max `arm64`), baseline (`2ed02ed`), C FFI |
| [baseline/bench-2026-10-30-mac-perf-ffi.csv](baseline/bench-2026-10-30-mac-perf-ffi.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`081c5d9`), C FFI |
| [baseline/bench-2026-10-30-mac-perf-direct.csv](baseline/bench-2026-10-30-mac-perf-direct.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`081c5d9`), Direct Rust API |
| [baseline/bench-2026-10-30-mac-m5max-ffi-post-opt.csv](baseline/bench-2026-10-30-mac-m5max-ffi-post-opt.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`05ee043`), C FFI |
| [baseline/bench-2026-10-30-mac-m5max-post-opt.csv](baseline/bench-2026-10-30-mac-m5max-post-opt.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`05ee043`), Direct Rust API |
| [baseline/bench-2026-10-30-android-base-ffi.csv](baseline/bench-2026-10-30-android-base-ffi.csv) | Android (`moto g play - 2023`, Cortex-A53 @ 2.30 GHz `cpu2`, 32-bit `armv7l`), baseline (`2ed02ed`), C FFI |
| [baseline/bench-2026-10-30-android-perf-ffi.csv](baseline/bench-2026-10-30-android-perf-ffi.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, 32-bit `armv7l`), `perf` (`081c5d9`), C FFI |
| [baseline/bench-2026-10-30-android-perf-direct.csv](baseline/bench-2026-10-30-android-perf-direct.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, 32-bit `armv7l`), `perf` (`081c5d9`), Direct Rust API |
| [baseline/bench-2026-10-30-android-armv7-ffi-post-opt.csv](baseline/bench-2026-10-30-android-armv7-ffi-post-opt.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, 32-bit `armv7l`), `perf` (`05ee043`), C FFI |
| [baseline/bench-2026-10-30-android-armv7-post-opt.csv](baseline/bench-2026-10-30-android-armv7-post-opt.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, 32-bit `armv7l`), `perf` (`05ee043`), Direct Rust API |

#### Decode throughput summary across platforms, build flags, and APIs (geomean)

| Platform & Config | `static standard` vs C (vs base) | `static dictionary` vs C (vs base) | `flushed standard` vs C (vs base) | `flushed dictionary` vs C (vs base) |
| --- | --- | --- | --- | --- |
| **`linux` i9-11900KF (native + fat LTO, 600 cells)** — Baseline (`2ed02ed`, FFI) | -4.6% | -8.5% | -6.2% | -9.8% |
| **`linux` i9-11900KF (native + fat LTO, 600 cells)** — `perf` (`081c5d9`, FFI) | **+1.8%** (+6.7%) | **+6.4%** (+16.7%) | **+2.7%** (+10.3%) | **+6.3%** (+19.2%) |
| **`linux` i9-11900KF (native + fat LTO, 144 cells)** — `perf` (`05ee043`, FFI) | **+2.6%** (**+6.6%**) | **+11.4%** (**+18.9%**) | **+4.1%** (**+10.1%**) | **+8.5%** (**+17.5%**) |
| **`linux` i9-11900KF (native + fat LTO, 144 cells)** — `perf` (`05ee043`, Direct) | **+2.3%** | **+10.8%** | **+3.9%** | **+7.8%** |
| **`linux` i9-11900KF (generic `x86-64` + ThinLTO, 144 cells)** — Baseline (`2ed02ed`, FFI) | -10.5% | -10.2% | -12.5% | -13.9% |
| **`linux` i9-11900KF (generic `x86-64` + ThinLTO, 144 cells)** — `perf` (`05ee043`, FFI) | -4.7% (**+7.4%**) | **+4.6%** (**+17.4%**) | -5.9% (**+9.4%**) | -0.7% (**+14.7%**) |
| **`linux` i9-11900KF (generic `x86-64` + ThinLTO, 144 cells)** — `perf` (`05ee043`, Direct) | -4.8% (**+7.3%**) | **+4.7%** (**+17.7%**) | -6.1% (**+9.1%**) | -1.3% (**+14.8%**) |
| **`mac` Apple M5 Max (`arm64`, 144 cells)** — Baseline (`2ed02ed`, FFI) | -0.3% | -6.9% | -6.5% | -11.9% |
| **`mac` Apple M5 Max (`arm64`, 144 cells)** — `perf` (`05ee043`, FFI) | **+10.4%** (**+10.3%**) | **+5.7%** (**+13.1%**) | **+8.1%** (**+15.7%**) | **+6.4%** (**+21.5%**) |
| **`mac` Apple M5 Max (`arm64`, 144 cells)** — `perf` (`05ee043`, Direct) | **+12.6%** (**+11.8%**) | **+9.4%** (**+16.4%**) | **+11.1%** (**+18.3%**) | **+10.8%** (**+25.7%**) |
| **Android Cortex-A53 (`armv7l` 32-bit, 144 cells)** — Baseline (`2ed02ed`, FFI) | -29.7% | -27.7% | -33.4% | -33.5% |
| **Android Cortex-A53 (`armv7l` 32-bit, 144 cells)** — `perf` (`081c5d9`, FFI) | -24.1% (**+9.1%**) | -16.7% (**+15.9%**) | -23.5% (**+15.3%**) | -19.7% (**+18.9%**) |
| **Android Cortex-A53 (`armv7l` 32-bit, 144 cells)** — `perf` (`05ee043`, FFI) | -21.5% (**+12.1%**) | -15.8% (**+17.2%**) | -19.6% (**+21.4%**) | -15.1% (**+28.9%**) |
| **Android Cortex-A53 (`armv7l` 32-bit, 144 cells)** — `perf` (`05ee043`, Direct) | -19.6% (**+15.1%**) | -12.2% (**+20.8%**) | -16.7% (**+26.4%**) | -10.0% (**+35.2%**) |
| **GCE Skylake-SP (`x86-64`, 300 cells)** — `perf` (`081c5d9`, FFI vs 2026-10-29 base) | -3.3% (**+7.7%**) | **+4.0%** (**+15.5%**) | -1.5% (**+12.0%**) | **+3.1%** (**+17.5%**) |

#### 1-byte fixed per-stream decode cost across platforms and APIs (`q6 static`)

| Platform | Dictionary | C (us) | Rust FFI (`081c5d9`, us) | Rust FFI (`05ee043`, us) | Rust Direct (`05ee043`, us) | FFI (`05ee043`) vs C | Direct (`05ee043`) vs C |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `linux` (i9-11900KF @ 3.5 GHz, 50k iters) | none | 0.33 | 0.44 | **0.32** | **0.26** | **-3% faster** | **-21% faster** |
| `linux` (i9-11900KF @ 3.5 GHz, 50k iters) | 64 KB (`wiki-html.dict`) | 0.40 | 0.46 | **0.33** | **0.28** | **-18% faster** | **-30% faster** |
| `mac` (Apple M5 Max, 50k iters) | none | 0.21 | 0.42 | **0.38** | **0.17** | +81% slower | **-19% faster** |
| `mac` (Apple M5 Max, 50k iters) | 64 KB (`wiki-html.dict`) | 0.21 | 0.25 | **0.25** | **0.12** | +19% slower | **-43% faster** |
| Android (Cortex-A53 @ 2.30 GHz, 10k iters) | none | 2.38 | 4.23 | **2.77** | **2.46** | +16% slower (**-35% vs `081c5d9`**) | +3% slower (**-20% vs `081c5d9`**) |
| Android (Cortex-A53 @ 2.30 GHz, 10k iters) | 64 KB (`wiki-html.dict`) | 2.46 | 4.31 | **2.85** | **2.69** | +16% slower (**-34% vs `081c5d9`**) | +9% slower (**-15% vs `081c5d9`**) |

### Observations (post-optimization)

1. **All 4 `(mode x case set)` columns beat C on `linux` (native) and `mac`
   (Apple M5 Max `arm64`), for both C FFI and Direct Rust API:**
   - On `linux` (144-cell default subset, `05ee043`): FFI reaches **`+2.6%`
     (static standard), `+11.4%` (static dictionary), `+4.1%` (flushed
     standard), and `+8.5%` (flushed dictionary) vs C**, matching Direct API
     (`+2.3% / +10.8% / +3.9% / +7.8%`).
   - On `mac` (Apple M5 Max `arm64`, 144 cells, `05ee043`): FFI reaches
     **`+10.4%` / `+5.7%` / `+8.1%` / `+6.4%` vs C** (`+10.3%` to `+21.5%`
     faster than baseline), and Direct API reaches **`+12.6%` / `+9.4%` /
     `+11.1%` / `+10.8%` vs C** (`+11.8%` to `+25.7%` faster than baseline;
     48/48 cells faster than C).
2. **FFI optimization (`fd8b2e8`) eliminates `SubclassableAllocator` drop-glue
   and state copy overhead, bringing C FFI to parity with Direct Rust API:**
   - Wrapping `MemoryBlock<Ty>`'s `Box<[Ty]>` in `ManuallyDrop` (`src/ffi/alloc_util.rs`),
     constructing `BrotliDecoderState` via `BrotliState::new_strict`, dropping in
     place via `core::ptr::drop_in_place` (`src/ffi/mod.rs`), and iterating only
     over `0..num_chunks` in `BrotliStateCleanup` (`src/state.rs`) cuts 1-byte
     FFI decode latency by **27–35%** (`0.44 -> 0.32 us` on `linux`, beating C's
     `0.33 us`; `4.23 -> 2.77 us` on Android Cortex-A53).
3. **32-bit bit-reader and ARMv7 bounds-check fast paths (`05ee043`) cut the
   Android Cortex-A53 gap by an additional 3–8 percentage points:**
   - Switching 32-bit targets to `reg_t = u32` (`BROTLI_SHORT_FILL_BIT_WINDOW_READ = 2`,
     `u16::from_le_bytes` on a 2-byte subslice, 32-bit `BitMask`, `#[cold]
     BrotliSafeReadBits32Slow`), using `u32` arithmetic in `ReadDistanceInternal`,
     and replacing panic indexing in Huffman subtables, `memmove16`, and
     `memcpy_within_slice` with `slice::get` / slice pattern matching brings
     Android Cortex-A53 (`armv7l`) total speedup over baseline (`2ed02ed`) to
     **`+12.1% / +17.2% / +21.4% / +28.9%` (FFI)** and **`+15.1% / +20.8% /
     +26.4% / +35.2%` (Direct)**.
   - **Correction (2026-10-31):** these Android binaries had no NEON on
     either side, and the gap is mostly the bit-reader representation, which
     safe Rust can fix (see the 2026-10-31 section below). Original text:
   - The remaining `-10.0%` to `-19.6%` gap on 32-bit ARMv7 (Cortex-A53) is
     structural to `#![forbid(unsafe_code)]` on an in-order 32-bit pipeline:
     safe Rust must bounds-check every bit-reader refill (`input.get`) and
     Huffman subtable lookup (`table.get`) against slice `.len()`, carry fat
     `(ptr, len)` slice pairs in a 15-register 32-bit register file, and
     zero-initialize (`calloc`) the ringbuffer slice before forming a safe `&mut [u8]`.

## Android ARMv7 with NEON (2026-10-31)

The 2026-10-30 Android CSVs were built without NEON on either side (see
[perf-host.md](perf-host.md)). This rerun of rust-brotli-decompressor
`05ee043` uses Chromium's arm32 configuration on both sides: Thumb-2 + NEON.
- C: `-O3 -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mthumb`.
- Rust: `thumbv7neon-unknown-linux-gnueabihf`, `-C target-cpu=cortex-a53`,
  fat LTO, `panic=abort`, stable 1.99.1.
- Run: `moto g play - 2023` (Cortex-A53), `taskset 4` (`cpu2`, 2.3 GHz),
  `brotli_compare --bench --decode-only --rust-api direct
  --min-time-ms 400`.
- CSV: `baseline/bench-2026-10-31-android-neon-direct.csv`.

Rust Direct vs C decode (geomean, 6 default cases, q0-q11):

| static standard | static dictionary | flushed standard | flushed dictionary |
|---:|---:|---:|---:|
| -19.5% | -10.9% | -16.3% | -9.9% |

That's within noise of the no-NEON run (-19.6% / -12.2% / -16.7% / -10.0%),
so NEON doesn't explain the gap. Copies do use NEON `vld1`/`vst1` now.

`simpleperf stat`, `wiki-Brotli.html` q5 static, 1000 iterations: C
3.08 G instructions / 3.33 G cycles; Rust 3.69 G (+20%) / 3.98 G, at the same
IPC (about 0.92). The gap is instruction count. See
[decoder-perf-plan.md](decoder-perf-plan.md#armv7-gap-2026-10-31) for where.

## Persistent `BrotliFastBitReader` & ARMv7 hot-loop optimizations: `45d09e0` (2026-10-31)

Measured after `vendor/rust-brotli-decompressor:perf` commit `45d09e0`
(persistent `BrotliFastBitReader` across `ProcessCommandsInternal`, branchless
`fill` without empty-slice store on underflow, hoisted `fill(16)` in
`read_bits`, `load_cmd_lut -> &'static CmdLutElement`, cached `[u8; 4]`
`dist_context_map`, hoisted counter updates out of the fast literal loops,
simplified `Option<[&LiteralTree; 64]>` `context_trees`, and hoisted
`memmove16` bounds check) on the 144-cell default benchmark subset across all
three platforms:

| Artifact | Platform & Config |
| --- | --- |
| [baseline/bench-2026-10-31-android-neon-direct-pbr5.csv](baseline/bench-2026-10-31-android-neon-direct-pbr5.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, Thumb-2 + NEON), `perf` (`45d09e0`), Direct Rust API |
| [baseline/bench-2026-10-31-linux-native-direct-pbr5.csv](baseline/bench-2026-10-31-linux-native-direct-pbr5.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`45d09e0`), Direct Rust API |
| [baseline/bench-2026-10-31-linux-native-ffi-pbr5.csv](baseline/bench-2026-10-31-linux-native-ffi-pbr5.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`45d09e0`), C FFI |
| [baseline/bench-2026-10-31-mac-m5max-direct-pbr5.csv](baseline/bench-2026-10-31-mac-m5max-direct-pbr5.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`45d09e0`), Direct Rust API |
| [baseline/bench-2026-10-31-mac-m5max-ffi-pbr5.csv](baseline/bench-2026-10-31-mac-m5max-ffi-pbr5.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`45d09e0`), C FFI |

| Platform & Config (144 cells, `45d09e0`) | `static standard` vs C (vs `05ee043`/`6082a30`) | `static dictionary` vs C (vs `05ee043`/`6082a30`) | `flushed standard` vs C (vs `05ee043`/`6082a30`) | `flushed dictionary` vs C (vs `05ee043`/`6082a30`) |
| --- | --- | --- | --- | --- |
| **Android Cortex-A53 (`thumbv7neon`, Direct)** | **-13.0%** (**+6.7%**) | **-5.3%** (**+7.9%**) | **-12.0%** (**+4.0%**) | **-6.1%** (**+3.9%**) |
| **`mac` Apple M5 Max (`arm64`, Direct)** | **+18.3%** (**+6.3%**) | **+15.9%** (**+7.3%**) | **+14.3%** (**+3.7%**) | **+14.3%** (**+3.8%**) |
| **`mac` Apple M5 Max (`arm64`, FFI)** | **+18.4%** (**+8.2%**) | **+15.2%** (**+9.8%**) | **+14.0%** (**+5.8%**) | **+12.7%** (**+5.5%**) |
| **`linux` i9-11900KF (native + fat LTO, Direct)** | **+3.6%** (+0.1%) | **+8.9%** (-0.4%) | **+5.0%** (-0.7%) | **+7.5%** (-0.5%) |
| **`linux` i9-11900KF (native + fat LTO, FFI)** | **+3.9%** (+0.5%) | **+10.5%** (-1.3%) | **+4.8%** (+0.3%) | **+8.4%** (-0.9%) |

`simpleperf stat` on Android Cortex-A53 (1000 iterations, `q5 static`, stable `rustc 1.99.1`):
- `wiki-Brotli.html`: C `3,082.8M` instructions / `3,297.8M` cycles (`160.5 MiB/s`); Rust `6082a30` `3,780.3M` / `4,004.4M` (`125.4 MiB/s`); **Rust `45d09e0` `3,573.3M` instructions (-206.9M, -29.7% of excess over C) / `3,783.9M` cycles (`136.4 MiB/s`, +8.8%)**.
- `jquery-3.7.1.min.js`: C `2,386.2M` instructions / `2,388.6M` cycles (`82.4 MiB/s`); Rust `6082a30` `2,818.9M` / `2,866.7M` (`68.4 MiB/s`); **Rust `45d09e0` `2,626.1M` instructions (-192.8M, -44.6% of excess over C) / `2,639.3M` cycles (`74.3 MiB/s`, +8.6%)**.

## ARMv7 16-bit unaligned loads, FFI single-zero alloc & loop register pressure: `d536284` (2026-10-31)

Measured after `vendor/rust-brotli-decompressor:perf` commit `d536284`
(`.get(..2).and_then(|s| s.try_into().ok())` for single `ldrh` 16-bit unaligned
refills on 32-bit targets, `#[inline(never)]` on `try_alloc_default_slice` so
32-bit FFI allocations fold to a single `calloc` without a redundant `memset`,
`#[cfg_attr(not(target_arch = "aarch64"), inline(never))]` on the 4 Huffman
tree-view helpers to prevent outer-loop LICM slice spills on 16-register `arm`
and `x86_64`, `#[inline(never)]` `CopyFromCustomSharedDictionary` taking `pos`
by value and `PostWrapCopy`, local `state` variable in `ProcessCommandsInternal`,
and simplified `ReplicateValue` + 32-bit `NextTableBitSize`) on the 144-cell
default benchmark subset across all three platforms:

| Artifact | Platform & Config |
| --- | --- |
| [baseline/bench-2026-10-31-android-neon-direct-pbr9.csv](baseline/bench-2026-10-31-android-neon-direct-pbr9.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, Thumb-2 + NEON), `perf` (`d536284`), Direct Rust API |
| [baseline/bench-2026-10-31-android-neon-ffi-pbr9.csv](baseline/bench-2026-10-31-android-neon-ffi-pbr9.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, Thumb-2 + NEON), `perf` (`d536284`), C FFI |
| [baseline/bench-2026-10-31-linux-native-direct-pbr9.csv](baseline/bench-2026-10-31-linux-native-direct-pbr9.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`d536284`), Direct Rust API |
| [baseline/bench-2026-10-31-linux-native-ffi-pbr9.csv](baseline/bench-2026-10-31-linux-native-ffi-pbr9.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`d536284`), C FFI |
| [baseline/bench-2026-10-31-mac-m5max-direct-pbr9.csv](baseline/bench-2026-10-31-mac-m5max-direct-pbr9.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`d536284`), Direct Rust API |
| [baseline/bench-2026-10-31-mac-m5max-ffi-pbr9.csv](baseline/bench-2026-10-31-mac-m5max-ffi-pbr9.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`d536284`), C FFI |

| Platform & Config (144 cells, `d536284`) | `static standard` vs C (vs `45d09e0`) | `static dictionary` vs C (vs `45d09e0`) | `flushed standard` vs C (vs `45d09e0`) | `flushed dictionary` vs C (vs `45d09e0`) |
| --- | --- | --- | --- | --- |
| **Android Cortex-A53 (`thumbv7neon`, Direct)** | **-6.4%** (**+7.8%**) | **+0.4%** (**+6.5%**) | **-5.5%** (**+7.5%**) | **-1.6%** (**+6.0%**) |
| **Android Cortex-A53 (`thumbv7neon`, FFI)** | **-7.0%** (**+18.5% vs `05ee043`**) | **-0.3%** (**+18.4% vs `05ee043`**) | **-6.0%** (**+16.9% vs `05ee043`**) | **-1.6%** (**+15.9% vs `05ee043`**) |
| **`linux` i9-11900KF (native + fat LTO, Direct)** | **+7.0%** (**+2.5%**) | **+14.5%** (**+3.7%**) | **+8.5%** (**+3.1%**) | **+11.6%** (**+3.1%**) |
| **`linux` i9-11900KF (native + fat LTO, FFI)** | **+7.0%** (**+2.4%**) | **+15.4%** (**+4.2%**) | **+8.5%** (**+3.0%**) | **+12.0%** (**+2.9%**) |
| **`mac` Apple M5 Max (`arm64`, Direct)** | **+15.5%** (-2.0%) | **+13.8%** (-1.3%) | **+13.5%** (-0.4%) | **+14.6%** (+1.3%) |
| **`mac` Apple M5 Max (`arm64`, FFI)** | **+20.0%** (**+1.5%**) | **+17.2%** (**+2.2%**) | **+15.9%** (**+1.9%**) | **+14.9%** (**+3.0%**) |

`simpleperf stat` on Android Cortex-A53 (1000 iterations, `q5 static`, stable `rustc 1.99.1`):
- `wiki-Brotli.html`: C `3,082.8M` instructions (`162.3 MiB/s`); Rust `45d09e0` Direct `3,489.6M` (`140.0 MiB/s`) -> **Rust `d536284` Direct `3,279.0M` instructions (-210.6M vs `45d09e0`, -500.3M vs `6082a30`, `154.7 MiB/s`)**; **Rust `d536284` FFI `3,284.8M` instructions (-76.5M from `#[inline(never)]` on `try_alloc_default_slice`, `155.4 MiB/s`)**.
- `jquery-3.7.1.min.js`: C `2,386.1M` instructions (`84.8 MiB/s`); Rust `45d09e0` Direct `2,585.8M` (`74.9 MiB/s`) -> **Rust `d536284` Direct `2,439.5M` instructions (-146.3M vs `45d09e0`, within 2.2% of C's instructions, `81.8 MiB/s`)**.
- `dict:jquery`: C `919.3M` instructions (`204.5 MiB/s`); **Rust `d536284` Direct `848.6M` instructions (`222.6 MiB/s`, +8.9% faster than C)**.

## 32-bit single-`ldr` `HuffmanCode` layout, 24-bit `n_bits <= 8` refills & branchless distance decode: `03ac41a` + `d889cc0` (2026-11-01)

Measured after `vendor/rust-brotli-decompressor:perf` commits `03ac41a` and
`d889cc0` (32-bit `{ bits, pad, value }` `HuffmanCode` layout + single-`ldr`
`load_huffman_code`, 32-bit 24-bit refill when `n_bits <= 8` with `bit_pos_ &=
7` + `#[cold]` `n_bits > 16` split read, metablock-invariant `npostfix == 0 &&
ndirect == 0` distance fast path at `q0..q9` + branchless `mask = !(distval >>
31) as u32` distance calculation at `q10..q11`, and `BrotliFastBitReader` /
`ProcessCommandsInternal` live-variable reductions) on the 144-cell default
benchmark subset across all three platforms:

| Artifact | Platform & Config |
| --- | --- |
| [baseline/bench-2026-10-31-android-neon-direct-pbr27.csv](baseline/bench-2026-10-31-android-neon-direct-pbr27.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, Thumb-2 + NEON), `perf` (`d889cc0`), Direct Rust API |
| [baseline/bench-2026-10-31-android-neon-ffi-pbr27.csv](baseline/bench-2026-10-31-android-neon-ffi-pbr27.csv) | Android (Cortex-A53 @ 2.30 GHz `cpu2`, Thumb-2 + NEON), `perf` (`d889cc0`), C FFI |
| [baseline/bench-2026-10-31-linux-native-direct-pbr27.csv](baseline/bench-2026-10-31-linux-native-direct-pbr27.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`d889cc0`), Direct Rust API |
| [baseline/bench-2026-10-31-linux-native-ffi-pbr27.csv](baseline/bench-2026-10-31-linux-native-ffi-pbr27.csv) | `linux` (i9-11900KF @ 3.5 GHz), native CPU + fat LTO, `perf` (`d889cc0`), C FFI |
| [baseline/bench-2026-10-31-mac-m5max-direct-pbr27.csv](baseline/bench-2026-10-31-mac-m5max-direct-pbr27.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`d889cc0`), Direct Rust API |
| [baseline/bench-2026-10-31-mac-m5max-ffi-pbr27.csv](baseline/bench-2026-10-31-mac-m5max-ffi-pbr27.csv) | `mac` (Apple M5 Max `arm64`), `perf` (`d889cc0`), C FFI |

| Platform & Config (144 cells, `d889cc0`) | `static standard` vs C (vs `d536284`) | `static dictionary` vs C (vs `d536284`) | `flushed standard` vs C (vs `d536284`) | `flushed dictionary` vs C (vs `d536284`) |
| --- | --- | --- | --- | --- |
| **Android Cortex-A53 (`thumbv7neon`, Direct)** | **-2.5%** (**+4.2%**) | **+4.7%** (**+4.3%**) | **-2.8%** (**+2.9%**) | **+2.0%** (**+3.7%**) |
| **Android Cortex-A53 (`thumbv7neon`, FFI)** | **-3.7%** (**+3.5%**) | **+3.4%** (**+3.7%**) | **-3.7%** (**+2.4%**) | **+0.9%** (**+2.5%**) |
| **`linux` i9-11900KF (native + fat LTO, Direct)** | **+6.6%** (-0.4%) | **+14.3%** (-0.2%) | **+8.5%** (0.0%) | **+11.5%** (-0.1%) |
| **`linux` i9-11900KF (native + fat LTO, FFI)** | **+5.4%** (-1.5%) | **+14.0%** (-1.2%) | **+7.5%** (-0.9%) | **+11.5%** (-0.4%) |
| **`mac` Apple M5 Max (`arm64`, Direct)** | **+22.0%** (**+5.6%**) | **+16.5%** (**+2.4%**) | **+17.6%** (**+3.6%**) | **+15.0%** (+0.3%) |
| **`mac` Apple M5 Max (`arm64`, FFI)** | **+21.1%** (**+0.9%**) | **+17.2%** (0.0%) | **+16.8%** (**+0.8%**) | **+14.8%** (-0.1%) |

`simpleperf stat` on Android Cortex-A53 (500 iterations, `taskset 4`, stable `rustc 1.99.1`):
- `wiki-Brotli.html` (`q5` static): C `1,541.4M` instructions / `22.70M` branch misses (`163.2 MiB/s`); **Rust `d889cc0` Direct `1,598.7M` instructions (within 3.7% of C, down from `1,890.1M` in `6082a30` and `1,639.5M` in `d536284`) / `24.13M` branch misses (`157.8 MiB/s`)**.
- `bootstrap-5.3.3.min.css` (`q6` static): C `1,304.9M` instructions / `16.59M` branch misses (`224.2 MiB/s`); **Rust `d889cc0` Direct `1,341.7M` instructions (within 2.8% of C, down from `1,426.3M` in `d536284`) / `16.80M` branch misses (`221.3 MiB/s`, within 1.3% of C)**.
- `wiki-Brotli.html` (`q11` static): C `5,253.7M` instructions / `45.16M` branch misses (`156.9 MiB/s`); **Rust `d889cc0` Direct `5,458.1M` instructions (within 3.9% of C, down from `5,571.6M` in `d536284`) / `45.47M` branch misses (-1.53M vs `pbr22`, matching C) (`144.3 MiB/s`, +5.8% faster than `pbr22`)**.
