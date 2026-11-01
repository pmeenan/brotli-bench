# ARMv7 decoder: next steps (handoff, 2026-10-31)

These are ideas for closing the remaining gap between the safe Rust decoder and C on 32-bit ARM. They're ranked by expected payoff and are meant to be picked up independently. Read [decoder-perf-plan.md](decoder-perf-plan.md#armv7-gap-2026-10-31) for the measurements behind them, and AGENTS.md for the rules: **no `unsafe`, decode output must not change**.

## Where things stand

- Fork: `vendor/rust-brotli-decompressor` `perf` at `d889cc0`.
- Android Cortex-A53, Thumb-2 + NEON on both sides, 144 cells (stable `rustc 1.99.1`):
  - Before `45d09e0` (`6082a30`): Direct -14.7% overall (-18.0% / -11.3% / -15.4% / -10.5% static std / static dict / flushed std / flushed dict); FFI (`05ee043`) -21.5% / -15.8% / -19.6% / -15.1%;
  - After `45d09e0`: Direct -10.3% overall (-13.0% / -5.3% / -12.0% / -6.1%);
  - After `d536284`: Direct -4.2% overall (-6.4% / +0.4% / -5.5% / -1.6%); FFI -7.0% / -0.3% / -6.0% / -1.6%;
  - After `03ac41a` + `d889cc0` (`pbr27`):
    - **Rust Direct vs C: `-0.7%` overall (`-2.5%` static std, `+4.7%` static dict, `-2.8%` flushed std, `+2.0%` flushed dict)** — **+4.2% / +4.3% / +2.9% / +3.7% faster than `d536284`**, **+10.7% faster than `45d09e0`**, and **+21.3% faster than `05ee043`**;
    - **Rust FFI vs C: `-1.8%` overall (`-3.7%` static std, `+3.4%` static dict, `-3.7%` flushed std, `+0.9%` flushed dict)**;
    - **Dictionary decode beats C on 32-bit ARMv7 in both static (`+4.7%` Direct, `+3.4%` FFI) and flushed (`+2.0%` Direct, `+0.9%` FFI) modes.**
- On `wiki-Brotli.html` q5 static (500 iterations, stable `rustc 1.99.1`), `03ac41a` + `d889cc0` cuts Rust Direct instructions from `1,890.1M` (`6082a30`) -> `1,744.8M` (`45d09e0`) -> `1,639.5M` (`d536284`) -> **`1,598.7M` (within 3.7% of C's `1,541.4M`, `157.8 MiB/s` vs C's `163.2 MiB/s`)**; on `bootstrap-5.3.3.min.css` q6 static (500 iterations), from `1,426.3M` (`d536284`) -> **`1,341.7M` (-84.6M, within 2.8% of C's `1,304.9M`, `221.3 MiB/s` vs C's `224.2 MiB/s`)**; and on `wiki-Brotli.html` q11 static (500 iterations), from `5,571.6M` / `46.99M` branch misses -> **`5,458.1M` instructions / `45.47M` branch misses (-113.5M instructions, -1.53M branch misses matching C's `45.16M`, `144.3 MiB/s`, +5.8% faster than `pbr22`)**.
- The remaining `-2.5%` to `-2.8%` non-dictionary gap on 32-bit ARMv7 matches the ~2.5–3% cost of **ring buffer zero-initialization (`calloc`/`memset`)**: safe Rust must zero-initialize the ring buffer slice before forming `&mut [u8]`, whereas C calls `malloc`.

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

### 3. 24-bit refill for `n_bits <= 8` on 32-bit — **LANDED (`d889cc0`)**

Restored C's 32-bit `n_bits <= 8` refill path in `BrotliFillBitWindowCompileTimeNbits` and `BrotliFastBitReader::fill`: when `n_bits <= 8` and `bit_pos_ >= 24`, shift `val_ >>= 24`, mask `bit_pos_ &= 7` (proving `bit_pos_ < 32` across both arms so LLVM elides the `and Rs, Rs, #31` shift-count mask on ARMv7), and load 3 bytes at once via a 32-bit unaligned load (`u32::from_le_bytes << 8`). Also marked the `n_bits > 16` split-read branch `#[cold]` via `cold_bit_reader_path()` in `BrotliReadBits` and `BrotliFastBitReader::read_bits`.

### 4. Copy path — **LANDED (`45d09e0`, `d536284`)**

- Hoisted the shared `len < 16 || max(off_dst, off_src) > len - 16` check in `memmove16` above the 64-bit / 32-bit `cfg` split, cutting the 32-bit `memmove16` bounds check from 10 instructions (4 branches) to 6 instructions (1 branch) (`45d09e0`).
- Extracted `PostWrapCopy` (`#[inline(never)]`) for the cold `BROTLI_STATE_COMMAND_POST_WRAP_COPY` state body so `copy_overlapping_backreference` and the byte-by-byte wrap loop do not clobber callee-saved registers in the hot `memmove16` path (`d536284`).

### 5. Ring buffer zero-fill (~3% on static streams) — **FFI DOUBLE-ZERO FIXED (`d536284`)**

- **FFI double zero-fill fixed (`d536284`)**: previously, inlining `try_alloc_default_slice` into `SubclassableAllocator::alloc_cell` caused SimplifyCFG on 32-bit ARM to tail-merge `try_alloc_default_slice`'s zero-fill loop with the custom `alloc_func` branch's zero-fill loop before LoopIdiomRecognize + InstCombine could fold `alloc_zeroed` + `memset` into `calloc`, causing the default FFI allocator to call `__rust_alloc_zeroed` (`calloc`) and then call `memset` on every ringbuffer and Huffman table allocation. Marking `try_alloc_default_slice` `#[inline(never)]` folds it to a single `__calloc` call (-76.5M instructions on `wiki-Brotli.html` q5 static FFI, bringing FFI within ~0.5% of Direct on Android).
- Remaining single zero-fill (~3% on static streams): safe Rust must zero the ring buffer once; C mallocs it. The harness's Android binaries are static glibc, not bionic, so allocator behavior there isn't the device's.

### 6. Huffman table building — **LANDED (`d536284`)**

- Rewrote `ReplicateValue` to step forward (`while let Some(slot) = slice.get_mut(pos) { *slot = code; pos += step_u; }`) and changed `NextTableBitSize` `left` from `i64` to `i32`, eliminating 64-bit arithmetic and per-iteration panic bounds checks in `BrotliBuildHuffmanTable` on 32-bit ARM (-22.1M instructions on `wiki-Brotli.html` q5 static).

### 7. Branch misses & distance decode — **LANDED (`d889cc0`)**

- In `ReadDistanceInternal` (`!safe`), added a fast path for the metablock-invariant `(s.distance_postfix_bits | (s.num_direct_distance_codes ^ 16)) == 0` case (`q0..q9`, -4.65M instructions on `bootstrap-5.3.3.min.css` q6 on Cortex-A53), and replaced the unpredictable `if distval >= 0` branch at `q10..q11` (`ndirect > 0 || npostfix > 0`) with a branchless `mask = !(distval >> 31) as u32` calculation (`nbits == 0` when `distval < 0`), eliminating **1.53M branch misses** on `wiki-Brotli.html` q11 on Cortex-A53 (`46.99M -> 45.47M`, matching C's `45.16M`, and boosting `wiki-Brotli.html` q11 decode throughput by **+5.8%**).

### 8. `HuffmanCode` layout (`BROTLI_HUFFMAN_CODE_FAST_LOAD`) — **LANDED (`03ac41a`)**

- Reordered `pub struct HuffmanCode` in `src/huffman/mod.rs` (and `HuffmanCodeStruct` in `c/brotli/decode.h`) on `#[cfg(not(target_pointer_width = "64"))]` to `{ pub bits: u8, pad: u8, pub value: u16 }` (`#[repr(C, align(4))]`), matching C's `BROTLI_HUFFMAN_CODE_FAST_LOAD` layout while keeping `{ pub value: u16, pub bits: u8 }` on 64-bit targets. In `load_huffman_code` on 32-bit targets, added a dummy volatile read on the unreachable `None` fallback arm so SimplifyCFG does not hoist the `ScalarPair(u8, u16)` loads above the bounds check before SROA + InstCombine merge them into a single `load i32, align 4`. Every Huffman lookup on 32-bit ARMv7 now compiles to a single `ldr.w Rd, [Rn, Rm, lsl #2]` followed by `uxtb`/`uxtab` (`bits`) and `lsr #16` (`value`), saving **85M–110M instructions** on `wiki-Brotli.html` and `bootstrap-5.3.3.min.css` on Cortex-A53 without any `unsafe` or API breakage.

## Not worth retrying

- `HuffmanCode { bits, value }` layout on 64-bit targets (`pbr10`): regresses x86-64 by ~1.5% and `aarch64` Direct by ~2%. Keep `{ value, bits }` on 64-bit targets.
- Precomputing `dist_extra_bits: [u8; 544]` + `dist_offset: [u32; 544]` in `BrotliState` (`CalculateDistanceLut`, `pbr24`): adds 2,720 bytes to `BrotliState`, pushing field offsets past Thumb-2's 12-bit immediate limit (`imm12 <= 4095`), forcing a second base register, spilling `s` and `br.rem`, and zeroing 2,720 extra bytes per stream (-2.4% on Android).
- Manual `#[repr(C)]` field ordering on `BrotliState` (`pbr25`): `repr(Rust)` already places all hot fields at offsets `< 3200` (`< 4095`) and 8-byte aligns the `HuffmanTreeGroup`s; `#[repr(C)]` perturbs alignment and spills `s` and `s.pos` (-1.1% on Android).
- `#[repr(C, align(8))]` on `PrefixCodeRange` (`kCmdLut`, `pbr20`): 8-byte `ldrd` requires even/odd register pairs on Thumb-2, increasing register spills in `ReadCommandInternal` (-2.2% on Android).
- Special-casing `n_bits == 0` in `BrotliFastBitReader::read_bits` (`pbr21`): adds a branch per command/distance (+11.2M instructions on `bootstrap` q6).
- Branchless `CopyOneByte` before `memmove16` for `i == 17` (`pbr16`), 8-byte NEON chunk loop in `memcpy_within_slice` (`pbr17`), or splitting overlapping back-references by `distance_code == 1` / `< 16` (`pbr18`): adds branches/code size in `ProcessCommandsInternal` (-1.3% to -2.3% on Android).
- Removing `#[inline(never)]` from the 4 Huffman tree-view helpers on `x86_64` (`pbr19`): regresses `linux` x86-64 by -1.2% to -2.2% due to LICM slice spills.
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
