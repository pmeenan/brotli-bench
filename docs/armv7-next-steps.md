# ARMv7 decoder: next steps (handoff, 2026-10-31)

These are ideas for closing the remaining gap between the safe Rust decoder and C on 32-bit ARM. They're ranked by expected payoff and are meant to be picked up independently. Read [decoder-perf-plan.md](decoder-perf-plan.md#armv7-gap-2026-10-31) for the measurements behind them, and AGENTS.md for the rules: **no `unsafe`, decode output must not change**.

## Where things stand

- Fork: `vendor/rust-brotli-decompressor` `perf` at `45d09e0` (`persistent-br` temporary branch merged and deleted).
- Android Cortex-A53, Thumb-2 + NEON on both sides, Rust Direct vs C, 144 cells (stable `rustc 1.99.1`):
  - Before `45d09e0` (`6082a30`): -14.7% overall (-18.0% / -11.3% / -15.4% / -10.5% static std / static dict / flushed std / flushed dict);
  - After `45d09e0`: **-10.3% overall (-13.0% / -5.3% / -12.0% / -6.1%)**, **+5.5% faster across all 144 cells** (+6.7% / +7.9% / +4.0% / +3.9%).
- On `wiki-Brotli.html` q5 static (1000 iterations, stable `rustc 1.99.1`), `45d09e0` cuts Rust instructions from `3,780.3M` to `3,573.3M` (**-206.9M instructions, -29.7% of the excess over C's `3,082.8M`**), and on `jquery-3.7.1.min.js` q5 static from `2,818.9M` to `2,626.1M` (**-192.8M instructions, -44.6% of the excess over C's `2,386.2M`**).
- Remaining ~16% instruction gap on `wiki-Brotli.html` q5 static is mostly:
  - **3-instruction Huffman table loads (`ldrh.w` + `add.w` + `ldrb`) vs C's 1-instruction `ldr` (`BROTLI_HUFFMAN_CODE_FAST_LOAD`)**: in `vendor/brotli-c/c/dec/huffman.h`, C defines `BROTLI_HUFFMAN_CODE_FAST_LOAD` on `ARMV7` / `ARMV8_32`, changing `HuffmanCode` from `struct { uint8_t bits; uint16_t value; }` to `typedef uint32_t HuffmanCode` (`(value << 16) | bits`). In Rust, `pub struct HuffmanCode { pub value: u16, pub bits: u8 }` is part of the public `AllocHC: Allocator<HuffmanCode>` trait bound, and `rustc` gives any 2-field struct `BackendRepr::ScalarPair(u16, u8)`, emitting separate `i16` and `i8` loads in LLVM IR even with `#[repr(C, align(4))]`. Because Thumb-2 `ldrb` has no scaled-index + displacement addressing mode, every Huffman lookup in Rust emits 3 instructions (`ldrh.w` + `add.w` + `ldrb`) vs 1 `ldr` in C (~120M–150M instructions across `wiki-Brotli.html` q5 static).
  - **Ring buffer zero-initialization (`memset`, ~3% of instructions)**: safe Rust must zero-initialize the ring buffer slice before forming `&mut [u8]`, whereas C calls `malloc`.
  - **Remaining slice length checks on 16-bit refills (`cmp r_len, #2`) and second-level Huffman subtable lookups (`table.get(idx)`)**.

## Tooling (all on the dev VM; the device hangs off `linux` via adb)

| Script | Purpose |
|---|---|
| `scripts/android/build.sh TAG` | Builds the working tree for the device (Thumb-2 + NEON) and pushes `brotli_compare_TAG` / `bench_rust_TAG`. `LOCAL=1` builds Rust locally with nightly `-Zbuild-std` (no `hostlock` wait). Compare only builds made with the same toolchain. |
| `scripts/android/bench.sh TAG NAME` | 144-cell decode-only bench, Rust Direct, on `cpu2`. The CSV lands in `/tmp/rv-arm/rv-NAME.csv`. About 2.7 min. |
| `scripts/android/compare.py A.csv B.csv` | Per-group C drift, Rust change, and the Rust/C ratio for both runs. Use the Rust/C ratio: the device drifts 1–3% between runs. |
| `scripts/android/attribute.py BIN ADDRS SYM OUTER` | Attributes `simpleperf report --sort vaddr_in_file` samples to call-site lines in `OUTER` (via the `addr2line -i` chain), and writes an annotated listing to `ADDRS.outer`. |
| `scripts/android/annotate.py BIN ADDRS SYM` | Per-instruction annotated disassembly. |
| `scripts/test_armv7.sh [--release]` | The crate's tests on armv7 under `qemu-arm` (perf host, `hostlock shared`). It's a per-commit gate. |

Quick A/B, deterministic enough for iteration: run `simpleperf stat -e instructions,cpu-cycles` on `bench_rust_TAG --rust-api direct --op decode --mode static --quality 5 --input corpora/wiki-Brotli.html --iterations 1000`, pinned with `taskset 4`.
- Record the profile with `simpleperf record -c 20000 -e instructions`, then `simpleperf report --sort vaddr_in_file`.
- `addr2line` loses inline info on the first address of a call, so `attribute.py` resolves all addresses in one batch.

Before committing, run the per-commit gates in [development.md](development.md). Also run the Android `--verify` with `taskset f0`; it takes about 10 min.

## Ideas, ranked

### 1. Persistent fast bit reader across `ProcessCommandsInternal` — **LANDED (`45d09e0`)**

**What landed:**
- `BrotliFastBitReader<'a>` in `src/bit_reader/mod.rs` (`new`, `store`, `avail_in`, `fill`, `get_bits`, `get_16_bits_unmasked`, `drop_bits`, `read_bits`), persisted across the non-safe path of `ProcessCommandsInternal` and synced to `s.br` only before safe-path calls, around rare block switches, and on exit.
- Critical fix in `BrotliFastBitReader::fill`: omitted the `else { self.tail = &[]; }` store on underflow, which previously forced LLVM to write `0` to `self.tail.len` and branch around the refill. Without it, a 16-bit refill compiles to a branchless Thumb-2 `ittt cs; ldrhcs; orrcs; subcs` sequence and keeps `tail.ptr` and `tail.len` in registers across `ReadCommandInternal` and the literal loops.
- Hoisted `self.fill(16)` before `if n_bits <= 16` in `BrotliFastBitReader::read_bits` and simplified the 32-bit `n_bits > 16` second refill (`self.bit_pos_ < 16` is already guaranteed after the first `fill(16)`).
- Hoisted `pos`, `block_length[0]`, and `i` counter updates out of both fast literal loops so the inner loops have no live-out counters.
- Also sped up Apple M5 Max (`arm64`) by +6.3% / +7.3% / +3.7% / +3.8% (Direct) and +8.2% / +9.8% / +5.8% / +5.5% (FFI), with zero x86-64 regression.

### 2. Per-command bounds checks — **PARTIALLY LANDED (`45d09e0`)**

- **`kCmdLut[cmd_code]`** (704 entries) — **LANDED (`45d09e0`)**: added `load_cmd_lut(index: usize) -> &'static prefix::CmdLutElement` (`None => &prefix::kCmdLut[0]`), which compiles to a branchless `cmp r0, #704; it cs; movcs r0, #0` and avoids SROA splitting `CmdLutElement` into 6 scalar live registers across `br.read_bits` (which previously spilled 3 registers in `ReadCommandInternal`). Also pre-loaded `v.insert_len_offset` and `v.copy_len_offset` before `br.read_bits`.
- **`dist_context_map[slice_index + distance_context]`** — **LANDED (`45d09e0`)**: added `load_dist_context_map -> [u8; 4]` cached in `ProcessCommandsInternal` and updated on `DecodeDistanceBlockSwitch`, indexed with `distance_context & 3`.
- **`context_trees` check in non-trivial literal loop** — **LANDED (`45d09e0`)**: simplified `context_trees` from `Option<([&LiteralTree; 64], usize)>` to `Option<[&LiteralTree; 64]>` invalidated on `DecodeLiteralBlockSwitch`.
- **`load_huffman_code`** second-level lookups on 32-bit (`table.get(idx)`): still open; could index the fixed-size `LiteralTree` / `CommandTree` arrays if `table_offset` is proven bounded.

### 3. 24-bit refill for `n_bits <= 8` on 32-bit

C refills 3 bytes when `bit_pos <= 8` for small peeks (`PreloadSymbol`, 8-bit `GetBits`). Rust dropped that path in `0466359` / `05ee043` and always refills 16 bits. Restoring it in the fast reader means fewer refills in the literal peek path. Measure; the literal loop is already cheaper than C's, so the gain may be small.

### 4. Copy path — **PARTIALLY LANDED (`45d09e0`)**

- Hoisted the shared `len < 16 || max(off_dst, off_src) > len - 16` check in `memmove16` above the 64-bit / 32-bit `cfg` split, cutting the 32-bit `memmove16` bounds check from 10 instructions (4 branches) to 6 instructions (1 branch).
- Compare with C's `memmove16` + `memcpy` split for copies over 32 bytes, and the overlap and wrap checks.
- `copy_bytes` (transform.rs) handles dictionary words.

### 5. Ring buffer zero-fill (~3% on static streams)

Safe Rust must zero the ring buffer; C mallocs it. Options:
- Check that the ring buffer is sized exactly like C's (`BrotliCalculateRingBufferSize`: the smallest size for a last metablock of known length).
- Use `alloc_zeroed` for large sizes, so fresh pages come pre-zeroed. That's allocator-dependent: glibc reuses heap chunks after the first stream, and Chromium uses PartitionAlloc.
- Grow the ring buffer lazily.
- The harness's Android binaries are static glibc, not bionic, so allocator behavior there isn't the device's. An NDK/bionic build would show the real cost.

### 6. Huffman table building

`BrotliBuildHuffmanTable` takes about 1.9% of Rust's time vs C's 0.9%; `ReadHuffmanCode` is about 1.4% vs 1.9%. This matters most for small and flushed streams. Look at `ReplicateValue` and the table-doubling loops on 32-bit.

### 7. Branch misses

Rust has 49.9M branch misses vs C's 42.9M (+16%) on wiki q5 static. At about 8 cycles each that's roughly 56M cycles, about 9% of the cycle gap. The A53's branch-miss sampling skids too much to localize them; try comparing the code structure around the literal and command loop exits and `mark_unlikely()` placement.

### 8. `HuffmanCode` layout (`BROTLI_HUFFMAN_CODE_FAST_LOAD`)

In `vendor/brotli-c/c/dec/huffman.h`, C defines `BROTLI_HUFFMAN_CODE_FAST_LOAD` on `ARMV7` / `ARMV8_32`, changing `HuffmanCode` from `struct { uint8_t bits; uint16_t value; }` to `typedef uint32_t HuffmanCode` (`(value << 16) | bits`) so a table entry loads in one `ldr` instruction. In Rust, `pub struct HuffmanCode { pub value: u16, pub bits: u8 }` is part of the public API (`AllocHC: Allocator<HuffmanCode>`), and `rustc` gives any 2-field struct `BackendRepr::ScalarPair(u16, u8)`, emitting separate `load i16` and `load i8` instructions in LLVM IR even with `#[repr(C, align(4))]`. Because Thumb-2 `ldrb` has no scaled-index + displacement addressing mode (`[base, idx, lsl #2] + 2`), every Huffman lookup in Rust emits 3 instructions (`ldrh.w` + `add.w` + `ldrb`) vs 1 `ldr` in C (~120M–150M instructions on `wiki-Brotli.html` q5 static). Fixing this without `unsafe` would require a single-field `pub struct HuffmanCode(pub u32)` representation (an API-breaking change to `HuffmanCode` field access).

## Not worth retrying

- A local `BrotliBitReader` copy inside the fast literal loops: no change.
- A per-call fast reader (idea 1 without persistence): +17% instructions.
- Returning `CmdLutElement` by value from `load_cmd_lut` (`pbr3`): SROA splits `v` into 6 scalar `phi` registers across `br.read_bits`, spilling `sl`, `r9`, and `ip` (+108M instructions vs returning `&'static CmdLutElement` in `pbr5`).
- Calling `fill_context_trees` eagerly in the `ProcessCommandsInternal` prologue (`pbr3b`): ran on every `ProcessCommandsInternal` invocation and increased outer-loop register pressure (+14M–16M instructions on `wiki-Brotli.html` and `jquery`). Keep `Option<[&LiteralTree; 64]>` lazy.
- Making `BitMask(32)` correct on 32-bit (`checked_shr`): +1.4% instructions. It's documented and `debug_assert!`ed as n <= 31 instead.
- From earlier sessions (AGENTS.md log):
  - an unconditional 64-bit refill;
  - `if let` state dispatch;
  - inlining `Decode*BlockSwitch`;
  - hoisting `br` / `state` to the top of `ProcessCommandsInternal` (x86 spills);
  - `#[inline]` on `BrotliStateCleanupAfterMetablock`.
