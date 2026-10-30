# Plan: Rust decoder performance

Status: **active, top priority.** The decoder is the short-term need. Encoder
work (parity follow-ups, encoder perf, and
[compound-dictionary-plan.md](compound-dictionary-plan.md)) is tabled until
the decoder reaches its target. Work happens on the `perf` branch of
`vendor/rust-brotli-decompressor` (see [development.md](development.md)).

## Goal

Make `rust-brotli-decompressor` decode as fast as the C decoder, or faster,
for Chrome-style streaming, without changing behavior:

- **Output and errors unchanged.** It must decode every valid stream
  byte-identically to C and reject the same invalid streams. Decode output is
  already identical to C on every verify case.
- **Workloads:**
  - static responses (16 KB network reads) and flushed/streamed responses
    (one wire chunk per read), with a 32 KB output buffer;
  - with and without raw dictionaries (Compression Dictionary Transport),
    including dictionaries larger than the window;
  - qualities 0-11, since servers use all of them.
- **Target:** on the perf host, geomean Rust/C decode throughput >= 1.00 in
  every (mode x case set) column of the baseline table, with no individual
  cell worse than -3%. Spot-check the final result on a second
  microarchitecture (the gap differed a lot between Skylake-SP and Rocket
  Lake).
- **Constraints:**
  - Keep the crate upstreamable to dropbox/rust-brotli-decompressor.
  - **No `unsafe`, period.** The decoder is going into Chromium for its
    memory-safety benefits. All performance work must be safe Rust:
    - The core crate builds with `forbid(unsafe_code)` in its default
      configuration. Every change must keep that build green.
    - Never enable or extend the crate's opt-in `unsafe` cargo feature
      (`get_unchecked` paths in `memory.rs`, `decode.rs`, `reader.rs` /
      `writer.rs`), and don't benchmark with it. The harness build enables
      only `std` + `ffi-api` (checked with `cargo tree -e features`).
    - The only existing `unsafe` in our build is the C ABI boundary in
      `src/ffi/` (`ffi-api` feature), which converts raw pointers. Don't add
      to it. If Chrome uses the Rust API directly (workstream 8), that code
      isn't even compiled.
    - If safe rewrites plateau short of C, document the remaining gap. Don't
      reach for `unsafe`.

## Where we are

**Post-optimization on `linux` (2026-10-30, `rust-brotli-decompressor` `perf` `081c5d9`; [baseline.md](baseline.md#post-optimization-decoder-benchmark-linux-2026-10-30)):**
i9-11900KF locked at 3.5 GHz. Rust vs C decode throughput, geomean
(positive = Rust faster):

| | Static (before -> after) | Flushed (before -> after) |
|---|---:|---:|
| Standard cases | -4.6% -> **+1.8%** (+6.7% vs baseline) | -6.2% -> **+2.7%** (+10.3% vs baseline) |
| Dictionary cases | -8.5% -> **+6.4%** (+16.7% vs baseline) | -9.8% -> **+6.3%** (+19.2% vs baseline) |

All 48 `(quality x mode x case set)` cells in the summary table meet the
`>= -3%` target: 47 of 48 are positive (`+0.9%` to `+10.6%`), and the single
negative cell is `static standard q11` at `-0.1%`.
- **Fixed cost per stream (1-byte input):** 0.34 us (C) vs 0.43 us (Rust, down
  from 0.46 us); with a 64 KB dictionary, 0.34 us (C) vs 0.45 us (Rust, down
  from 0.49 us).
- **GCE Skylake-SP spot-check (300 cells vs 2026-10-29 baseline):**
  - `static standard`: **+7.7% vs baseline** (-11.6% -> -3.3% vs C);
  - `static dictionary`: **+15.5% vs baseline** (-10.5% -> **+4.0% vs C**);
  - `flushed standard`: **+12.0% vs baseline** (-13.5% -> -1.5% vs C);
  - `flushed dictionary`: **+17.5% vs baseline** (-12.6% -> **+3.1% vs C**).

**Pre-optimization perf host baseline `linux` ([baseline.md](baseline.md#perf-host-baseline-linux-2026-10-30)):**
i9-11900KF locked at 3.5 GHz (`rust-brotli-decompressor` `2ed02ed`). Rust vs C
decode throughput, geomean (negative = Rust slower):

| | Static | Flushed |
|---|---:|---:|
| Standard cases | -4.6% | -6.2% |
| Dictionary cases | -8.5% | -9.8% |

On that host:
- **q0-q2:** -6 to -8%.
- **q3-q9:** -4 to -7% standard, -9 to -11% with a dictionary.
- **q10/q11:** -0.5 to -4% standard, -8 to -11% with a dictionary.
- **Fixed cost per stream:** 0.34 us (C) vs 0.46 us (Rust).

The dictionary decode gap is about twice the standard gap there, so
workstream 6 matters more than its position in the list suggests.

**GCE dev VM, 2026-10-29 (Skylake-SP; history):** a larger gap, which shows
the result depends on the microarchitecture.

| | Static | Flushed |
|---|---:|---:|
| Standard cases | -11.6% | -13.5% |
| Dictionary cases | -12.9% | -14.8% |

By quality (standard cases):
- **q0-q2:** -14 to -18% (literal-heavy, simple prefix codes).
- **q3-q9:** -10 to -14%.
- **q10/q11:** -7.5 to -11%.
- **With a dictionary, q10/q11:** -14 to -16.5%.

Other gaps:
- **Fixed cost per stream** (1-byte input: create, attach, decode,
  destroy): C 0.50 us vs Rust 0.69 us; with a 64 KB dictionary 0.56 vs
  0.73 us.
- **Setup-dominated cases are the worst:** `monkey` (843 B) is -23 to -28%,
  and the bootstrap delta (193-byte stream) -24 to -27%.
- **Flushed per-chunk latency** is 2-5 us higher (about 10-20%).

### First profiles (2026-10-30)

`perf record` (cpu-clock sampling: the GCE VM has no hardware PMU events),
`bench_c` vs `bench_rust --op decode`, pinned to CPU 6:

| Workload | C MiB/s | Rust MiB/s | Hot spots, C / Rust (% of samples) |
|---|---:|---:|---|
| wiki q1 static | 346 | 297 (-14%) | `ProcessCommands` 92.7 / 91.8; memmove 4.3 / 4.2; **memset — / 1.2** |
| wiki q5 static | 383 | 342 (-11%) | `ProcessCommands` 87.2 / 87.1 |
| wiki q5 flushed | 269 | 236 (-12%) | `ProcessCommands` 59.4 / 56.8; `ReadHuffmanCode` 14.1 / 13.4; `BuildHuffmanTable` 7.4 / **8.9**; `SafeProcessCommands` 4.2 / 3.8; **memset — / 1.8**; **`SafeReadSymbol` — / 1.7** |
| jquery q11 static | 176 | 158 (-10%) | `ProcessCommands` 79.9 / 79.6 |

Reading:
- **Both decoders spend their time in the same places, in the same
  proportions.** The gap is not a missing algorithm. It is extra work spread
  across the hot command loop: likely bounds checks, bit-reader and copy
  codegen, and branch layout.
- **The Rust decoder does extra zeroing** (memset, 1-2%) and slightly more
  Huffman-table building, which matters in flushed mode. There, about 20% of
  the time goes to per-meta-block prefix-code setup.
- **The Rust safe-path symbol reads** (`SafeReadSymbol`) show up in flushed
  mode. The decoder drops out of the fast path more often near chunk
  boundaries.

## Method

- **Per change:** `brotli_compare --verify` must exit 0 (decode parity
  600/600).
  - The default-feature build of the decompressor must compile, since it
    enforces `forbid(unsafe_code)`: `cargo build --release` in
    `vendor/rust-brotli-decompressor`.
  - No added `unsafe` anywhere:
    `git diff upstream/master...perf | grep '^+.*unsafe'` must be empty.
  - Also run `cargo test --release` in the decompressor.
  - Run the decompressor's fuzz targets (`vendor/rust-brotli-decompressor/fuzz`)
    for any change to bit reading, Huffman decoding, or copy logic.
- **Measure on the physical `linux` host** under `hostlock exclusive`
  ([perf-host.md](perf-host.md)). Record a host-specific baseline first.
- **Measure decode only.** It is stable to about 0.3% between runs (see
  baseline.md), so 1-2% wins are measurable.
  - Quick loop: `bench_rust --op decode` on a few representative cases,
    pinned.
  - Before/after:
    `brotli_compare --bench --set all --corpus ... --mode all --bench-csv`
    restricted to decode, or the default subset. Then
    `scripts/summarize_bench.py --baseline`.
  - Record results in the AGENTS.md log, including failed attempts.
- **Profiling:**
  - `perf record -g` / `perf annotate` on `bench_rust` vs `bench_c` for the
    same case. Compare hot loops instruction by instruction.
  - The dev VM has no hardware counters or valgrind. The `linux` host has
    PMU counters (`perf stat -e instructions:u,cycles:u`), so instruction
    counts and IPC are available there.
- **Representative quick set:**
  - `wiki-Brotli.html` q1/q5/q11, static and flushed;
  - `jquery-3.7.1.min.js` q5/q11;
  - `dict:jquery-3.6.4-to-3.7.1.min.js` q5/q11;
  - `dict:wiki-html-Brotli` q5;
  - a 1-byte input (fixed cost).

## Workstreams (in priority order)

1. **Hot command loop (`ProcessCommandsInternal`, `decode.rs`) — DONE (`081c5d9`, `0466359`).**
   - Introduced fixed-size array types (`LiteralTree = [HuffmanCode; 632]`,
     `CommandTree = [HuffmanCode; 1080]`, `HuffmanRootTable = [HuffmanCode; 256]`)
     so tree pointers are thin 8-byte references and 8-bit root-table lookups
     have no slice bounds checks.
   - Replaced the 1024-byte zeroed `[&[HuffmanCode]; 64]` `context_trees`
     array with `Option<([&LiteralTree; 64], usize)> = None`, populated only
     when non-trivial literal contexts are used.
   - Added fast slice-based inner literal loops for both trivial and
     non-trivial literal contexts, hoisted `literal_htree` across commands,
     and jumped directly back to the fast literal loop after
     `DecodeLiteralBlockSwitch` when slack allows.
   - Matched C's block-switch inlining structure (`#[inline(never)]` on
     `Decode*BlockSwitch` / `SafeDecode*BlockSwitch` and `#[inline(always)]`
     on `DecodeBlockTypeAndLength`, `PrepareLiteralDecoding`, and
     `Decode*BlockSwitchInternal`) so `safe` and `tree_type` constant-fold
     without bloating `ProcessCommandsInternal`.
   - Added a `17..=32` byte overlapping copy arm to `transform::copy_bytes`
     (`0466359`), bound `s.ringbuffer.slice_mut()` once in backward copy and
     wrap-copy states, and eliminated 6 dead `MemoryBlock::drop` checks when
     restoring `saved_*_hgroup` on exit from `ProcessCommandsInternal`.
2. **Bit reader (`bit_reader/mod.rs`) — DONE (`0466359`).**
   - Unified `BrotliFillBitWindow` with `BrotliFillBitWindowCompileTimeNbits`.
3. **Prefix-code setup (`ReadHuffmanCode`, `BrotliBuildHuffmanTable`,
   `ReplicateValue`, context maps) — DONE (`1344c34`).**
   - Replaced signed modulo (`idiv`) in `ReplicateValue` with a power-of-two
     bitmask and hoisted a single subslice bounds check (`table.get_mut`).
   - Simplified `symbol_list_value` to a single unsigned bounds check.
   - Optimized `BrotliBuildSimpleHuffmanTable` with slice fill for 0-symbol
     tables and `split_at_mut` + `clone_from_slice` for table doubling.
   - Passed fixed-size array references into `ProcessSingleCodeLength` and
     `ProcessRepeatedCodeLength`, used slice fill for `symbols_lists_array`,
     and hoisted bounds checks out of `InverseMoveToFrontTransform`,
     `DecodeContextMapInner`, and `DetectTrivialLiteralBlockTypes`.
4. **Fixed per-stream costs — DONE (`1344c34`, `081c5d9`).**
   - 1-byte decode time improved from 0.46 to 0.43 us (0.49 to 0.45 us with a
     64 KB dictionary) on `linux`.
   - Note: LBR profiling shows the remaining ~1% `__memset_evex_unaligned_erms`
     on static streams is `calloc` zero-filling the ringbuffer slice and initial
     Huffman `codes` slice, which is required by safe Rust's initialized slice
     invariant.
5. **Streaming / flushed path — DONE (`1344c34`, `081c5d9`).**
   - Reused `context_map`, `context_modes`, `dist_context_map`, and
     `HuffmanTreeGroup` (`htrees` and `codes`) across metablocks instead of
     freeing and reallocating them on every metablock in flushed mode.
   - Marked `SafeReadSymbol` and `ReadBlockLength` `#[inline(always)]`.
   - Flushed decode geomean improved from -6.2% / -9.8% vs C to +2.7% / +6.3%
     vs C on `linux`.
6. **Dictionary decode — DONE (`081c5d9`).**
   - Added an inline single-chunk compound dictionary fast path in
     `ProcessCommandsInternal` using `transform::copy_bytes` when
     `s.compound_dictionary.num_chunks == 1`, and skipped
     `EnsureCompoundDictionaryInitialized` for single-chunk dictionaries.
   - Dictionary decode geomean improved from -8.5% (static) / -9.8% (flushed)
     vs C to +6.4% (static) / +6.3% (flushed) vs C on `linux`.
7. **Build configuration realism (open).**
   - The baseline uses `-C target-cpu=native` and fat LTO. Chromium ships
     generic x86-64 (and arm64) builds with its own Rust flags.
   - Add a measurement with Chromium-like flags: no `target-cpu=native`,
     Chromium's opt-level and LTO settings. Keep the comparison like-for-like
     by building C with matching flags.
   - Wins must hold under those flags.
8. **Integration path check (open).** Confirm which API Chrome will call: the
   C FFI (what the harness measures) or the Rust API directly. Measure that
   path too, if it differs. The FFI wrapper itself is about 2% in profiles.

## Deliverables and upstreaming

- Small, self-contained commits on `rust-brotli-decompressor:perf`, each
  with before/after numbers in the message.
- Push the fork branch, then bump the submodule here (development.md).
- Batch related commits into upstream PRs to
  dropbox/rust-brotli-decompressor once a workstream shows a clear win.
- Add a new dated measurement section to baseline.md when the decoder reaches
  parity. Never rewrite the 2026-10-29 baseline.

## Open questions

- Which flags and API Chrome will actually use (workstreams 7 and 8). Ask
  before optimizing for `target-cpu=native`-specific codegen.
