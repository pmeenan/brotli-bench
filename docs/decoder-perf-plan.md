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

1. **Hot command loop (`ProcessCommandsInternal`, `decode.rs`) — DONE (`081c5d9`, `0466359`, `05ee043`).**
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
   - Switched `ReadDistanceInternal` distance arithmetic to `u32` (saving 20M
     instructions on `x86_64` and avoiding 64-bit sign-extension/shifts on
     32-bit ARMv7), added `load_huffman_code` using `slice::get` on 32-bit
     targets to eliminate panic bounds checks on Huffman subtable lookups, and
     used slice pattern matching / `slice::get` in `memmove16` and
     `memcpy_within_slice` on 32-bit targets (`05ee043`).
2. **Bit reader (`bit_reader/mod.rs`) — DONE (`0466359`, `05ee043`).**
   - Unified `BrotliFillBitWindow` with `BrotliFillBitWindowCompileTimeNbits`
     (`0466359`).
   - Added a dedicated 32-bit bit-reader configuration on non-64-bit targets
     (`reg_t = u32`, `BROTLI_SHORT_FILL_BIT_WINDOW_READ = 2`, 16-bit unaligned
     refills via `u16::from_le_bytes` on a 2-byte subslice, 32-bit `BitMask`,
     and `#[cold]` out-of-line `BrotliSafeReadBits32Slow` for `n_bits > 16`)
     matching C's `BROTLI_64_BITS == 0` path (`05ee043`).
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
4. **Fixed per-stream costs — DONE (`1344c34`, `081c5d9`, `fd8b2e8`).**
   - Eliminated `SubclassableAllocator` (`MemoryBlock<Ty>`) drop-glue loops by
     wrapping its `Box<[Ty]>` in `ManuallyDrop`, constructed `BrotliDecoderState`
     directly via `BrotliState::new_strict` in `BrotliDecoderCreateInstance`,
     dropped in place via `core::ptr::drop_in_place` in
     `BrotliDecoderDestroyInstance` (eliminating a ~7 KB stack copy), and
     iterated only over `0..num_chunks` in `BrotliStateCleanup` (`fd8b2e8`).
   - 1-byte FFI decode time on `linux` dropped from `0.46 -> 0.32 us` (`64 KB
     dict`: `0.49 -> 0.33 us`), beating C (`0.33 / 0.40 us`); Direct API reaches
     `0.26 / 0.28 us`. On Android Cortex-A53 (`armv7l`), 1-byte FFI decode
     dropped from `4.23 -> 2.77 us` (`-35%`).
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
     vs C on `linux` (600 cells) and +4.1% / +8.5% on the 144-cell subset.
6. **Dictionary decode — DONE (`081c5d9`).**
   - Added an inline single-chunk compound dictionary fast path in
     `ProcessCommandsInternal` using `transform::copy_bytes` when
     `s.compound_dictionary.num_chunks == 1`, and skipped
     `EnsureCompoundDictionaryInitialized` for single-chunk dictionaries.
   - Dictionary decode geomean improved from -8.5% (static) / -9.8% (flushed)
     vs C to +6.4% (static) / +6.3% (flushed) vs C on `linux` (600 cells) and
     +11.4% / +8.5% on the 144-cell subset.
7. **Build configuration realism — DONE.**
   - Added `-DBROTLI_BENCH_C_ARCH_FLAGS=""` to `CMakeLists.txt` so C and Rust can
     be built with Chromium-like generic x86-64 flags (`-DBROTLI_BENCH_C_ARCH_FLAGS=""
     -DBROTLI_BENCH_RUSTFLAGS="" -DBROTLI_BENCH_RUST_LTO=thin`).
   - On `linux` (i9-11900KF @ 3.5 GHz, 144-cell default subset), the `perf`
     (`05ee043`) speedup over baseline (`2ed02ed`) holds under generic x86-64 +
     ThinLTO: `+7.4%` static standard (`-10.5% -> -4.7% vs C`), `+17.4%` static
     dictionary (`-10.2% -> +4.6% vs C`), `+9.4%` flushed standard
     (`-12.5% -> -5.9% vs C`), `+14.7%` flushed dictionary (`-13.9% -> -0.7% vs C`).
   - Also tested replacing `BitMask(n)`'s shift expression with a `kBitMask`
     lookup on non-BMI2 `x86_64`; it was within noise (`+0.6%` static standard,
     `-0.6%` flushed dictionary) and was reverted.
8. **Integration path check & FFI optimization (C FFI vs direct Rust API) — DONE (`fd8b2e8`, `05ee043`).**
   - Added `RustDirectBrotliApi` (`rust/lib.rs`, `src/streaming_harness.h`) and
     `--rust-api <ffi|direct>` (`brotli_compare`, `bench_rust`), then optimized
     the built-in C FFI (`src/ffi/alloc_util.rs`, `src/ffi/mod.rs`,
     `src/state.rs`) in `fd8b2e8` so C FFI callers get nearly identical
     performance to Direct Rust API callers.
   - **1-byte fixed per-stream decode cost (`05ee043`):**
     - `linux` (i9-11900KF @ 3.5 GHz): C `0.33 us` (`0.40 us` w/ 64 KB dict),
       **Rust FFI `0.32 us` (`0.33 us`) — faster than C**, **Rust Direct
       `0.26 us` (`0.28 us`) — 21–30% faster than C**.
     - `mac` (Apple M5 Max): C `0.21 us` (`0.21 us`), Rust FFI `0.38 us`
       (`0.25 us`), **Rust Direct `0.17 us` (`0.12 us`) — 19–43% faster than C**.
     - Android (Cortex-A53 @ 2.30 GHz, 32-bit `armv7l`): C `2.38 us`
       (`2.46 us`), **Rust FFI `2.77 us` (`2.85 us`) — 35% lower latency than
       `081c5d9` (`4.23 / 4.31 us`)**, **Rust Direct `2.46 us` (`2.69 us`)**.
   - **Full streaming decode throughput (`05ee043`, 144-cell default subset):**
     - `linux` (native + fat LTO): FFI is **`+2.6%` static standard, `+11.4%`
       static dictionary, `+4.1%` flushed standard, `+8.5%` flushed dictionary
       vs C**; Direct is **`+2.3% / +10.8% / +3.9% / +7.8%` vs C** (FFI and
       Direct are now at parity).
     - `mac` (Apple M5 Max `arm64`): FFI is **`+10.4% / +5.7% / +8.1% / +6.4%`
       vs C** (`+10.3%..+21.5%` vs baseline `2ed02ed`); Direct is **`+12.6% /
       +9.4% / +11.1% / +10.8%` vs C** (`+11.8%..+25.7%` vs baseline).
     - Android (Cortex-A53 32-bit `armv7l`): FFI improved to `-21.5% / -15.8% /
       -19.6% / -15.1%` vs C (**`+12.1% / +17.2% / +21.4% / +28.9%` faster than
       baseline `2ed02ed` FFI**); Direct improved to `-19.6% / -12.2% / -16.7% /
       -10.0%` vs C (**`+15.1% / +20.8% / +26.4% / +35.2%` faster than baseline
       `2ed02ed` FFI**). (Corrected 2026-10-31: the gap is mostly the 32-bit
       bit-reader refill, fixable in safe Rust; see "ARMv7 gap" below. Original
       claim follows.) The remaining gap on 32-bit in-order ARMv7 is structural
       to `#![forbid(unsafe_code)]`: slice bounds checks on every bit-reader
       refill (`input.get`) and Huffman subtable lookup (`table.get`) plus fat
       `(ptr, len)` slice references on a 15-register 32-bit file and mandatory
       `calloc` zero-initialization of the ringbuffer.

## ARMv7 gap (2026-10-31)

Measured on Android Cortex-A53, Thumb-2 + NEON on both sides
([baseline.md](baseline.md#android-armv7-with-neon-2026-10-31)). Instruction
samples were attributed to their call site in `ProcessCommandsInternal`
through the `addr2line -i` inline chain:

| Phase | Rust vs C instructions |
|---|---:|
| `ReadCommand` (symbol + extra bits) | +26% |
| `ReadDistance` | +23% |
| Trivial-context literal loop | -26% |
| Ring buffer `memset` (zero-init; C mallocs) | ~3% of Rust time |

**Root cause:** the 32-bit window refills every 16 bits.
- C's refill is about 8 instructions: post-increment `ldrh [next_in], #2`,
  shift, or; there's no separate `avail_in`.
- Rust's is about 13-21: an `input.get(next_in..)` check against
  `input.len()`, an `avail_in` load/sub/store, the input pointer and length
  reloaded from the stack, and `s.br` field traffic.
- `kCmdLut` and `dist_context_map` add 3-instruction bounds checks per
  command.

None of this needs `unsafe` to fix.

Tried and reverted:
1. A local `BrotliBitReader` copy in the fast literal loops. The loop became
   register-resident, but there was no change (those loops aren't the gap).
2. A per-call tail-slice reader (`BrotliFastBitReader`: the unread input as a
   subslice, `len()` = `avail_in`) in `ReadCommand`, `ReadDistance` and the
   literal loops. The refill matched C's about 8 instructions, but
   constructing and storing the reader on every call made it +17%
   instructions and -11% speed.

**Landed (`45d09e0`):** a persistent `BrotliFastBitReader` across the non-safe
path of `ProcessCommandsInternal` (without the redundant empty-slice store on
underflow in `fill`, and with `self.fill(16)` hoisted in `read_bits`), plus
`load_cmd_lut -> &'static CmdLutElement`, cached `[u8; 4]` `dist_context_map`,
hoisted counter updates out of the fast literal loops, simplified
`Option<[&LiteralTree; 64]>` `context_trees`, and a hoisted `memmove16` bounds
check.
- Cuts **206.9M instructions (-29.7% of excess over C)** on `wiki-Brotli.html`
  q5 static (`3,780.3M -> 3,573.3M`, `+8.8%` median throughput) and **192.8M
  instructions (-44.6% of excess over C)** on `jquery-3.7.1.min.js` q5 static
  (`2,818.9M -> 2,626.1M`, `+8.6%` median throughput) on Cortex-A53.
- Improves the 144-cell Android Cortex-A53 benchmark by **+6.7% / +7.9% /
  +4.0% / +3.9%** (+5.5% overall across all 144 cells), narrowing the Rust
  Direct vs C gap from `-14.7%` to **`-10.3%`** (`-13.0% / -5.3% / -12.0% /
  -6.1%`).
- Also speeds up Apple M5 Max (`arm64`) by another **+6.3% / +7.3% / +3.7% /
  +3.8% (Direct)** and **+8.2% / +9.8% / +5.8% / +5.5% (FFI)** to **`+18.3% /
  +15.9% / +14.3% / +14.3%` (Direct)** and **`+18.4% / +15.2% / +14.0% / +12.7%`
  (FFI)** vs C, while keeping `linux` i9-11900KF ahead of C (`+3.6% / +8.9% /
  +5.0% / +7.5%` Direct, `+3.9% / +10.5% / +4.8% / +8.4%` FFI).

**Landed (`d536284`):**
- Single `ldrh` 16-bit unaligned loads on 32-bit targets via
  `.get(..2).and_then(|s| s.try_into().ok())` in `BrotliLoad16LE`,
  `BrotliFastBitReader::fill`, and `BrotliFastBitReader::read_bits` (slice
  destructuring emitted two `ldrb`s + shifts and prevented LLVM from combining
  them into `ldrh`; `-80M` to `-110M` instructions on `wiki-Brotli.html` q5
  static on Cortex-A53).
- `#[inline(never)]` on `try_alloc_default_slice` in `src/ffi/alloc_util.rs`
  so 32-bit FFI allocations fold to a single `calloc` instead of tail-merging
  with the custom allocator branch and calling `calloc` + `memset` (`-76.5M`
  instructions on `wiki-Brotli.html` q5 static FFI on Cortex-A53, bringing FFI
  within ~0.5% of Direct).
- `#[cfg_attr(target_arch = "aarch64", inline(always))]` +
  `#[cfg_attr(not(target_arch = "aarch64"), inline(never))]` on `huffman_tree`,
  `distance_root_table`, `literal_huffman_tree`, and `command_huffman_tree` to
  prevent outer-loop LICM slice spills on 16-register `arm` and `x86_64`, plus
  `#[inline(never)]` on `InitializeCompoundDictionaryCopy`,
  `CopyFromCompoundDictionary`, `CopyFromCustomSharedDictionary` (taking `pos`
  by value), and `PostWrapCopy`, local `state` in `ProcessCommandsInternal`,
  and simplified `ReplicateValue` + 32-bit `NextTableBitSize` (`-22.1M`
  instructions in `BrotliBuildHuffmanTable`).
- Cuts another **210.6M instructions** on `wiki-Brotli.html` q5 static
  (`3,489.6M -> 3,279.0M`, `-500.3M` vs `6082a30`, `154.7 MiB/s` Direct /
  `155.4 MiB/s` FFI vs C's `3,082.8M` / `162.3 MiB/s`) and **146.3M
  instructions** on `jquery-3.7.1.min.js` q5 static (`2,585.8M -> 2,439.5M`,
  within 2.2% of C's `2,386.1M` instructions, `81.8 MiB/s` vs C's `84.8 MiB/s`).
- Improves the 144-cell Android Cortex-A53 benchmark by another **+7.8% / +6.5% /
  +7.5% / +6.0%** over `45d09e0` (**+14.1% overall over `05ee043`**), bringing
  **Rust Direct vs C to `-6.4%` static std, `+0.4%` static dict, `-5.5%` flushed
  std, `-1.6%` flushed dict** (`-4.2%` overall) and **Rust FFI vs C to `-7.0% /
  -0.3% / -6.0% / -1.6%`**. The remaining `-5.5%` to `-6.4%` non-dictionary gap
  is smaller than the combined ~7% cost of safe Rust's mandatory ringbuffer
  zero-initialization (~3%) and C's `BROTLI_HUFFMAN_CODE_FAST_LOAD` single-`ldr`
  `HuffmanCode` layout (~4%, which requires a breaking `pub struct HuffmanCode`
  change).
- Also improves `linux` i9-11900KF by another **+2.5% to +4.2%** to **`+7.0% /
  +14.5% / +8.5% / +11.6%` (Direct)** and **`+7.0% / +15.4% / +8.5% / +12.0%`
  (FFI)** vs C, and `mac` M5 Max FFI to **`+20.0% / +17.2% / +15.9% / +14.9%`**
  vs C (`+15.5% / +13.8% / +13.5% / +14.6%` Direct).

**Landed (`03ac41a`, `d889cc0`):**
- **Single-`ldr` `HuffmanCode` layout on 32-bit targets (`03ac41a`):** reordered
  `pub struct HuffmanCode` in `src/huffman/mod.rs` (and `HuffmanCodeStruct` in
  `c/brotli/decode.h`) on `#[cfg(not(target_pointer_width = "64"))]` to
  `{ pub bits: u8, pad: u8, pub value: u16 }` (`#[repr(C, align(4))]`), matching
  C's `BROTLI_HUFFMAN_CODE_FAST_LOAD` layout while keeping `{ pub value: u16,
  pub bits: u8 }` on 64-bit targets, and outlined `load_huffman_code_into` (no
  `#[inline]`) using `clone_from_slice` in `load_huffman_code` so LLVM's CGSCC
  pass folds the 4-byte `memcpy` into a single `load i32, align 4` before SROA.
  Every Huffman lookup on 32-bit ARMv7 now compiles to a single
  `ldr.w Rd, [Rn, Rm, lsl #2]` followed by `uxtb`/`uxtab` (`bits`) and `lsr #16`
  (`value`) instead of 3 instructions (`ldrh.w` + `add.w` + `ldrb`), saving
  **85M–110M instructions** on `wiki-Brotli.html` and `bootstrap-5.3.3.min.css`.
- **32-bit 24-bit refill for `n_bits <= 8` & `#[cold]` `n_bits > 16` path
  (`d889cc0`):** restored C's 32-bit `n_bits <= 8` refill path in
  `BrotliFillBitWindowCompileTimeNbits` and `BrotliFastBitReader::fill` (`val_
  >>= 24`, `bit_pos_ &= 7`, `u32::from_le_bytes << 8`, where `& 7` proves
  `bit_pos_ < 32` across both arms so LLVM elides the `and Rs, Rs, #31`
  shift-count mask on ARMv7), and marked the `n_bits > 16` split-read branch
  `#[cold]`.
- **Fast `npostfix == 0 && ndirect == 0` distance path + branchless `q10-q11`
  distance decode (`d889cc0`):** added a fast path in `ReadDistanceInternal`
  (`!safe`) for `(s.distance_postfix_bits | (s.num_direct_distance_codes ^ 16))
  == 0` (`q0..q9`, -4.65M instructions on `bootstrap-5.3.3.min.css` q6 on
  Cortex-A53) and a branchless `mask = !(distval >> 31) as u32` calculation for
  `ndirect > 0 || npostfix > 0` (`q10..q11`), eliminating **1.53M branch
  misses** on `wiki-Brotli.html` q11 on Cortex-A53 (`46.99M -> 45.47M`, matching
  C's `45.16M`, and boosting `wiki-Brotli.html` q11 decode throughput by
  **+5.8%**). Also dropped the redundant `start_len` field from
  `BrotliFastBitReader` and moved `compound_dictionary_size` into the dictionary
  branch of `ProcessCommandsInternal`.
- Improves the 144-cell Android Cortex-A53 benchmark by another **+4.2% / +4.3% /
  +2.9% / +3.7%** over `d536284` (**+21.3% overall over `05ee043`**), bringing
  **Rust Direct vs C to `-2.5%` static std, `+4.7%` static dict, `-2.8%` flushed
  std, `+2.0%` flushed dict** (`-0.7%` overall) and **Rust FFI vs C to `-3.7% /
  +3.4% / -3.7% / +0.9%`** (beating C on dictionary streams in both static and
  flushed modes; the remaining `-2.5%` to `-2.8%` non-dictionary gap matches the
  ~2.5–3% cost of mandatory safe-Rust ringbuffer zero-initialization).
- Also improves `mac` M5 Max Direct to **`+22.0% / +16.5% / +17.6% / +15.0%`**
  vs C (`+21.1% / +17.2% / +16.8% / +14.8%` FFI) and keeps `linux` i9-11900KF at
  **`+6.6% / +14.3% / +8.5% / +11.5%` (Direct)** and **`+5.4% / +14.0% / +7.5% /
  +11.5%` (FFI)** vs C.

**Landed (`32e41d6`):**
- Generalized the metablock-invariant `s.distance_postfix_bits == 0` fast path
  in `ReadDistanceInternal` to handle any `ndirect` via
  `code.checked_sub(ndirect)` (`bootstrap-5.3.3.min.css` at `q11` has
  `NPOSTFIX = 0, NDIRECT = 1` and previously missed the fast path 100% of the
  time).
- Unified the `len < 32 || max(off_dst, off_src) > len - 32` bounds check in
  `memmove16_from_ringbuffer` and `memmove16_offset16`, matched a 32-byte slice
  prefix at `off_src`/`off_dst` in `memmove16_offset16`, and checked bounds +
  non-overlap via `split_at_mut` in safe `memcpy_within_slice`, allowing LLVM GVN
  on 32-bit ARMv7 to eliminate the second bounds check on `17 <= copy_len <= 32`
  and all `dst_start`/`lr` spills across the first 16-byte copy (shrinking the
  `17 <= copy_len <= 32` branch from 20 instructions + 3 stack ops to 5
  instructions).
- Added `prev_index -> prev_tree` run caching in `fill_context_trees`.
- Cuts **`136,715` instructions/decode (`-4.8%`)** on `bootstrap-5.3.3.min.css`
  `q11` on Cortex-A53 (`2,853,918 -> 2,717,203`), improving `bootstrap` static
  `q11` from `-13.9%` vs C to **`-8.3%` vs C** on Android (`flushed q11` `-9.5% ->
  -6.0%`), from `-7.5%` vs C to **`-0.7%` vs C** on `linux` i9-11900KF, and
  reaching **`+18.0%` vs C** (`+15.4%` static overall) on `mac` M5 Max.

**Landed (`afe3536`):**
- **Review hardening:** restored the 28-byte fast-path margin re-check (`if !safe
  && br.avail_in() < 28 { result = BROTLI_DECODER_NEEDS_MORE_INPUT; break; }`)
  after `DecodeCommandBlockSwitch` in `ProcessCommandsInternal`, added
  `debug_assert!(false, ...)` on underflow in `BrotliFastBitReader::fill` and
  `BrotliFastBitReader::read_bits`, and fixed 32-bit `BrotliReadConstantNBits`
  for `n` in `25..=32` (`BrotliFillBitWindowCompileTimeNbits(br, 16, input)`).
- **Huffman setup (`src/huffman/mod.rs`, `src/decode.rs`):** replaced
  `actual_count[1..] != count[1..]` in `BrotliBuildCodeLengthsHuffmanTable` with
  an XOR-fold loop (eliminating the per-tree `__memcmpeq`/`bcmp` call), added
  `replicate_value_pow2` (without `#[inline]` so LLVM's CGSCC pass coerces the
  4-byte `HuffmanCode` ABI argument to `i32` before inlining on 32-bit ARM,
  emitting a single `str.w` per table slot) using two-step
  `table.get_mut(offset..)?.get_mut(..=last)` slicing, replaced the runtime
  `s.symbol_lists_index` load with `SYMBOL_LISTS_OFFSET` (`16`) and `.get_mut` in
  `ProcessSingleCodeLength` / `ProcessRepeatedCodeLength`, and used
  `BrotliFastBitReader` (`br.fill(17)`) in `ReadSymbolCodeLengths`.
- **`SafeProcessCommands` (`src/bit_reader/mod.rs`, `src/decode.rs`):** marked
  `BrotliGetAvailableBits`, `BrotliPullByte` (via `input.get`),
  `BrotliSafeGetBits`, `BrotliSafeReadBits`, and `SafeReadBits`
  `#[inline(always)]`, marked `SafeDecodeSymbol` `#[cold]`, used cached
  `dist_context_map[(context & 3) as usize]` in `ReadCommandInternal` (`safe`),
  removed dead `!safe` branches in `ReadDistanceInternal` (`safe`), and skipped
  `distance_root_table` updates when `safe == true`.
- Cuts another **`27K–98K` instructions/decode** across `bootstrap` and
  `wiki-Brotli.html` (`static` and `flushed`, `q5` and `q11`) on Cortex-A53,
  bringing **all three benchmark platforms ahead of C overall across all 144
  cells** (`docs/baseline/bench-2026-11-01-*-pbr35.csv`):
  - **Android Cortex-A53 (`armv7l`, Thumb-2 + NEON):**
    - **Rust Direct vs C:** **`+0.77%` overall** (`-1.3%` static std, `+5.5%`
      static dict, `-0.8%` flushed std, `+3.5%` flushed dict; **`+0.9%` static
      overall, `+0.6%` flushed overall**).
    - **Rust FFI vs C:** **`+1.16%` overall** (`-1.0%` static std, `+6.1%` static
      dict, `-0.4%` flushed std, `+3.8%` flushed dict; **`+1.3%` static overall,
      `+1.0%` flushed overall**).
  - **Linux i9-11900KF (@ 3.5 GHz, `x86_64`):**
    - **Rust Direct vs C:** **`+9.90%` overall** (`+7.5%` static std, `+16.1%`
      static dict, `+8.2%` flushed std, `+12.2%` flushed dict).
    - **Rust FFI vs C:** **`+8.57%` overall** (`+6.2%` static std, `+13.9%`
      static dict, `+7.3%` flushed std, `+10.7%` flushed dict).
  - **Apple M5 Max (`aarch64`):**
    - **Rust Direct vs C:** **`+23.17%` overall** (`+25.1%` static std, `+20.3%`
      static dict, `+23.5%` flushed std, `+21.7%` flushed dict).
    - **Rust FFI vs C:** **`+17.38%` overall** (`+20.8%` static std, `+17.1%`
      static dict, `+15.6%` flushed std, `+14.6%` flushed dict).

## Deliverables and upstreaming

- Small, self-contained commits on `rust-brotli-decompressor:perf`, each
  with before/after numbers in the message (`0466359`, `1344c34`, `081c5d9`,
  `fd8b2e8`, `05ee043`, `6082a30`, `45d09e0`, `d536284`, `03ac41a`,
  `d889cc0`, `32e41d6`, `afe3536`).
- Push the fork branch, then bump the submodule here (development.md).
- Batch related commits into upstream PRs to
  dropbox/rust-brotli-decompressor once a workstream shows a clear win.
- Add a new dated measurement section to baseline.md when the decoder reaches
  parity. Never rewrite the 2026-10-29 baseline.
