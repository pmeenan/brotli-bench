# ARMv7 decoder: next steps (handoff, 2026-10-31)

These are ideas for closing the remaining gap between the safe Rust decoder and C on 32-bit ARM. They're ranked by expected payoff and are meant to be picked up independently. Read [decoder-perf-plan.md](decoder-perf-plan.md#armv7-gap-2026-10-31) for the measurements behind them, and AGENTS.md for the rules: **no `unsafe`, decode output must not change**.

## Where things stand

- Fork: `vendor/rust-brotli-decompressor` `perf` at `d536284`.
- Android Cortex-A53, Thumb-2 + NEON on both sides, 144 cells (stable `rustc 1.99.1`):
  - Before `45d09e0` (`6082a30`): Direct -14.7% overall (-18.0% / -11.3% / -15.4% / -10.5% static std / static dict / flushed std / flushed dict); FFI (`05ee043`) -21.5% / -15.8% / -19.6% / -15.1%;
  - After `45d09e0`: Direct -10.3% overall (-13.0% / -5.3% / -12.0% / -6.1%);
  - After `d536284`:
    - **Rust Direct vs C: `-4.2%` overall (`-6.4%` static std, `+0.4%` static dict, `-5.5%` flushed std, `-1.6%` flushed dict)** — **+7.8% / +6.5% / +7.5% / +6.0% faster than `45d09e0`** and **+14.1% faster than `05ee043`**;
    - **Rust FFI vs C: `-7.0%` static std, `-0.3%` static dict, `-6.0%` flushed std, `-1.6%` flushed dict** (within ~0.5% of Direct).
- On `wiki-Brotli.html` q5 static (1000 iterations, stable `rustc 1.99.1`), `d536284` cuts Rust Direct instructions from `3,780.3M` (`6082a30`) -> `3,489.6M` (`45d09e0`) -> **`3,279.0M` (-500.3M total, within 6.4% of C's `3,082.8M`, `154.7 MiB/s` Direct / `155.4 MiB/s` FFI vs C's `162.3 MiB/s`)**, and on `jquery-3.7.1.min.js` q5 static from `2,818.9M` (`6082a30`) -> `2,585.8M` (`45d09e0`) -> **`2,439.5M` (-379.4M total, within 2.2% of C's `2,386.1M` instructions, `81.8 MiB/s` vs C's `84.8 MiB/s`)**. On `dict:jquery` q5 static, Rust Direct is `848.6M` instructions (`222.6 MiB/s`, **+8.9% faster than C's `919.3M` / `204.5 MiB/s`**).
- The remaining `-5.5%` to `-6.4%` non-dictionary gap on 32-bit ARMv7 is smaller than the combined ~7% cost of:
  - **3-instruction Huffman table loads (`ldrh.w` + `add.w` + `ldrb`) vs C's 1-instruction `ldr` (`BROTLI_HUFFMAN_CODE_FAST_LOAD`, ~4% of instructions)**: in `vendor/brotli-c/c/dec/huffman.h`, C defines `BROTLI_HUFFMAN_CODE_FAST_LOAD` on `ARMV7` / `ARMV8_32`, changing `HuffmanCode` from `struct { uint8_t bits; uint16_t value; }` to `typedef uint32_t HuffmanCode` (`(value << 16) | bits`). In Rust, `pub struct HuffmanCode { pub value: u16, pub bits: u8 }` is part of the public `AllocHC: Allocator<HuffmanCode>` trait bound, and `rustc` gives any 2-field struct `BackendRepr::ScalarPair(u16, u8)`, emitting separate `i16` and `i8` loads in LLVM IR even with `#[repr(C, align(4))]`. Because Thumb-2 `ldrb` has no scaled-index + displacement addressing mode, every Huffman lookup in Rust emits 3 instructions (`ldrh.w` + `add.w` + `ldrb`) vs 1 `ldr` in C (~120M–150M instructions across `wiki-Brotli.html` q5 static).
  - **Ring buffer zero-initialization (`calloc`/`memset`, ~3% of instructions)**: safe Rust must zero-initialize the ring buffer slice before forming `&mut [u8]`, whereas C calls `malloc`.

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

### 1. Persistent fast bit reader & 16-bit `ldrh` refills across `ProcessCommandsInternal` — **LANDED (`45d09e0`, `d536284`)**

**What landed:**
- `BrotliFastBitReader<'a>` in `src/bit_reader/mod.rs` (`new`, `store`, `avail_in`, `fill`, `get_bits`, `get_16_bits_unmasked`, `drop_bits`, `read_bits`), persisted across the non-safe path of `ProcessCommandsInternal` and synced to `s.br` only before safe-path calls, around rare block switches, and on exit (`45d09e0`).
- Critical fix in `BrotliFastBitReader::fill`: omitted the `else { self.tail = &[]; }` store on underflow, which previously forced LLVM to write `0` to `self.tail.len` and branch around the refill. Without it, a 16-bit refill compiles to a branchless Thumb-2 `ittt cs; ldrhcs; orrcs; subcs` sequence and keeps `tail.ptr` and `tail.len` in registers across `ReadCommandInternal` and the literal loops (`45d09e0`).
- Replaced slice destructuring (`if let &[a, b, ..] = ...`) in 32-bit `BrotliLoad16LE`, `BrotliFastBitReader::fill`, and `BrotliFastBitReader::read_bits` (`n_bits > 16` second refill) with `.get(..2).and_then(|s| s.try_into().ok())` (`d536284`). On `rustc 1.99.1` (LLVM 22) for `thumbv7neon`, slice destructuring emitted two `ldrb`s + shifts and prevented LLVM from combining them into `ldrh`; `.get(..2).and_then(|s| s.try_into().ok())` emits `load i16, align 1` directly in LLVM IR and compiles to post-indexed `ldrhcs.w r0, [r9], #2` (-80M to -110M instructions on `wiki-Brotli.html` q5 static).
- Hoisted `self.fill(16)` before `if n_bits <= 16` in `BrotliFastBitReader::read_bits` and simplified the 32-bit `n_bits > 16` second refill (`self.bit_pos_ < 16` is already guaranteed after the first `fill(16)`).
- Hoisted `pos`, `block_length[0]`, and `i` counter updates out of both fast literal loops so the inner loops have no live-out counters.
- Outlined the 4 Huffman tree-view helpers (`huffman_tree`, `distance_root_table`, `literal_huffman_tree`, `command_huffman_tree`) on non-`aarch64` targets (`#[cfg_attr(not(target_arch = "aarch64"), inline(never))]`), outlined `InitializeCompoundDictionaryCopy`, `CopyFromCompoundDictionary`, `CopyFromCustomSharedDictionary` (taking `pos` by value instead of `&mut pos`), and `PostWrapCopy`, and kept `state` in a local variable across `ProcessCommandsInternal` (`d536284`).

### 2. Per-command bounds checks — **LANDED (`45d09e0`, `d536284`)**

- **`kCmdLut[cmd_code]`** (704 entries) — **LANDED (`45d09e0`)**: added `load_cmd_lut(index: usize) -> &'static prefix::CmdLutElement` (`None => &prefix::kCmdLut[0]`), which compiles to a branchless `cmp r0, #704; it cs; movcs r0, #0` and avoids SROA splitting `CmdLutElement` into 6 scalar live registers across `br.read_bits` (which previously spilled 3 registers in `ReadCommandInternal`). Also pre-loaded `v.insert_len_offset` and `v.copy_len_offset` before `br.read_bits`.
- **`dist_context_map[slice_index + distance_context]`** — **LANDED (`45d09e0`)**: added `load_dist_context_map -> [u8; 4]` cached in `ProcessCommandsInternal` and updated on `DecodeDistanceBlockSwitch`, indexed with `distance_context & 3`.
- **`context_trees` check in non-trivial literal loop** — **LANDED (`45d09e0`)**: simplified `context_trees` from `Option<([&LiteralTree; 64], usize)>` to `Option<[&LiteralTree; 64]>` invalidated on `DecodeLiteralBlockSwitch`.

### 3. 24-bit refill for `n_bits <= 8` on 32-bit

C refills 3 bytes when `bit_pos <= 8` for small peeks (`PreloadSymbol`, 8-bit `GetBits`). Rust dropped that path in `0466359` / `05ee043` and always refills 16 bits. Restoring it in the fast reader means fewer refills in the literal peek path. Measure; the literal loop is already cheaper than C's, so the gain may be small.

### 4. Copy path — **LANDED (`45d09e0`, `d536284`)**

- Hoisted the shared `len < 16 || max(off_dst, off_src) > len - 16` check in `memmove16` above the 64-bit / 32-bit `cfg` split, cutting the 32-bit `memmove16` bounds check from 10 instructions (4 branches) to 6 instructions (1 branch) (`45d09e0`).
- Extracted `PostWrapCopy` (`#[inline(never)]`) for the cold `BROTLI_STATE_COMMAND_POST_WRAP_COPY` state body so `copy_overlapping_backreference` and the byte-by-byte wrap loop do not clobber callee-saved registers in the hot `memmove16` path (`d536284`).

### 5. Ring buffer zero-fill (~3% on static streams) — **FFI DOUBLE-ZERO FIXED (`d536284`)**

- **FFI double zero-fill fixed (`d536284`)**: previously, inlining `try_alloc_default_slice` into `SubclassableAllocator::alloc_cell` caused SimplifyCFG on 32-bit ARM to tail-merge `try_alloc_default_slice`'s zero-fill loop with the custom `alloc_func` branch's zero-fill loop before LoopIdiomRecognize + InstCombine could fold `alloc_zeroed` + `memset` into `calloc`, causing the default FFI allocator to call `__rust_alloc_zeroed` (`calloc`) and then call `memset` on every ringbuffer and Huffman table allocation. Marking `try_alloc_default_slice` `#[inline(never)]` folds it to a single `__calloc` call (-76.5M instructions on `wiki-Brotli.html` q5 static FFI, bringing FFI within ~0.5% of Direct on Android).
- Remaining single zero-fill (~3% on static streams): safe Rust must zero the ring buffer once; C mallocs it. The harness's Android binaries are static glibc, not bionic, so allocator behavior there isn't the device's.

### 6. Huffman table building — **LANDED (`d536284`)**

- Rewrote `ReplicateValue` to step forward (`while let Some(slot) = slice.get_mut(pos) { *slot = code; pos += step_u; }`) and changed `NextTableBitSize` `left` from `i64` to `i32`, eliminating 64-bit arithmetic and per-iteration panic bounds checks in `BrotliBuildHuffmanTable` on 32-bit ARM (-22.1M instructions on `wiki-Brotli.html` q5 static).

### 7. Branch misses

Rust has 49.9M branch misses vs C's 42.9M (+16%) on wiki q5 static. At about 8 cycles each that's roughly 56M cycles, about 9% of the cycle gap. The A53's branch-miss sampling skids too much to localize them; try comparing the code structure around the literal and command loop exits and `mark_unlikely()` placement.

### 8. `HuffmanCode` layout (`BROTLI_HUFFMAN_CODE_FAST_LOAD`)

In `vendor/brotli-c/c/dec/huffman.h`, C defines `BROTLI_HUFFMAN_CODE_FAST_LOAD` on `ARMV7` / `ARMV8_32`, changing `HuffmanCode` from `struct { uint8_t bits; uint16_t value; }` to `typedef uint32_t HuffmanCode` (`(value << 16) | bits`) so a table entry loads in one `ldr` instruction. In Rust, `pub struct HuffmanCode { pub value: u16, pub bits: u8 }` is part of the public API (`AllocHC: Allocator<HuffmanCode>`), and `rustc` gives any 2-field struct `BackendRepr::ScalarPair(u16, u8)`, emitting separate `load i16` and `load i8` instructions in LLVM IR even with `#[repr(C, align(4))]`. Because Thumb-2 `ldrb` has no scaled-index + displacement addressing mode (`[base, idx, lsl #2] + 2`), every Huffman lookup in Rust emits 3 instructions (`ldrh.w` + `add.w` + `ldrb`) vs 1 `ldr` in C (~120M–150M instructions on `wiki-Brotli.html` q5 static). Fixing this without `unsafe` would require a single-field `pub struct HuffmanCode(pub u32)` representation (an API-breaking change to `HuffmanCode` field access).

## Not worth retrying

- A local `BrotliBitReader` copy inside the fast literal loops: no change.
- A per-call fast reader (idea 1 without persistence): +17% instructions.
- Returning `CmdLutElement` by value from `load_cmd_lut` (`pbr3`): SROA splits `v` into 6 scalar `phi` registers across `br.read_bits`, spilling `sl`, `r9`, and `ip` (+108M instructions vs returning `&'static CmdLutElement` in `pbr5`).
- Calling `fill_context_trees` eagerly in the `ProcessCommandsInternal` prologue (`pbr3b`): ran on every `ProcessCommandsInternal` invocation and increased outer-loop register pressure (+14M–16M instructions on `wiki-Brotli.html` and `jquery`). Keep `Option<[&LiteralTree; 64]>` lazy.
- Outlining `ReadCommandInternal` or `ReadDistanceInternal` (`#[inline(never)]`), or splitting the fast literal loops into separate `#[inline(never)]` functions: passing `&mut BrotliFastBitReader` across call boundaries spills its 4 registers on every command / literal run (+153M to +158M instructions).
- Caching `htrees[0]` as a fixed-size `&[HuffmanCode; 1080]` slice in the trivial literal loop: extra outer-loop live value spills 2 registers (+135M instructions).
- Outlining the 4 Huffman tree-view helpers unconditionally on `aarch64`: costs ~2.5% on Direct on 32-register Apple M5 Max where LICM does not spill. Keep `#[cfg_attr(target_arch = "aarch64", inline(always))]`.
- Making `BitMask(32)` correct on 32-bit (`checked_shr`): +1.4% instructions. It's documented and `debug_assert!`ed as n <= 31 instead.
- From earlier sessions (AGENTS.md log):
  - an unconditional 64-bit refill;
  - `if let` state dispatch;
  - inlining `Decode*BlockSwitch`;
  - hoisting `br` to the top of `ProcessCommandsInternal` (x86 spills);
  - `#[inline]` on `BrotliStateCleanupAfterMetablock`.
