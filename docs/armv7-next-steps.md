# ARMv7 decoder: next steps (handoff, 2026-10-31)

These are ideas for closing the remaining gap between the safe Rust decoder and C on 32-bit ARM. They're ranked by expected payoff and are meant to be picked up independently. Read [decoder-perf-plan.md](decoder-perf-plan.md#armv7-gap-2026-10-31) for the measurements behind them, and AGENTS.md for the rules: **no `unsafe`, decode output must not change**.

## Where things stand

- Fork: `vendor/rust-brotli-decompressor` `perf` at `6082a30`.
- Android Cortex-A53, Thumb-2 + NEON on both sides, Rust Direct vs C, 144 cells:
  - with the perf host's stable toolchain: -19.5% / -10.9% / -16.3% / -9.9% (static standard / static dictionary / flushed standard / flushed dictionary);
  - with the local nightly: about -14.7% overall.
- On `wiki-Brotli.html` q5 static, Rust runs **+20% instructions** at the same IPC as C. Attributing instruction samples to their call site in `ProcessCommandsInternal`:
  - `ReadCommand` +26%;
  - `ReadDistance` +23%;
  - copies about 2x C's;
  - the trivial-context literal loop is already 26% *cheaper* than C;
  - ring buffer `memset` is about 3% of Rust's time and absent in C.
- Root cause: on 32-bit the bit window refills every 16 bits.
  - Rust's refill is about 13–21 instructions: an `input.get(next_in..)` check against `input.len()`, an `avail_in` load/sub/store, input pointer and length reloaded from the stack, and `s.br` field traffic.
  - C's is about 8: post-increment `ldrh [next_in], #2`, shift, or, and no `avail_in`.
  - 64-bit refills about 3x less often, which is why x86-64 and Apple M5 are already ahead of C.

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

### 1. Persistent fast bit reader across `ProcessCommandsInternal` (biggest lever)

**Idea.** Keep the unread input as a subslice `tail: &[u8]` for the whole call, so `tail.len()` *is* `avail_in`. A refill becomes "length check, `ldrh`, pointer bump"; there's no separate `avail_in` and no `next_in` vs `input.len()` check. Sync back to `s.br` only at the exit and around calls that use `s.br`.

**Evidence.**
- A per-call version, constructed and stored in each `ReadCommand` / `ReadDistance` / literal loop, did get the refill down to C's ~8 instructions.
- But the construct/store overhead on every call made it +17% instructions overall.

**WIP.** Local branch `persistent-br` in the fork (`8c5cbad`, **not pushed, doesn't build**). It has:
- `BrotliFastBitReader` (`new`, `store`, `avail_in`, `fill`, `get_bits`, `get_16_bits_unmasked`, `drop_bits`, `read_bits`), mirroring the `BrotliBitReader` functions bit for bit, for both register widths;
- `FastReadSymbol`, `FastPreloadSymbol`, `FastReadPreloadedSymbol`;
- `ReadCommandInternal` / `ReadDistanceInternal` taking `br: &mut BrotliFastBitReader` for the non-safe path.

**Remaining work, all in `ProcessCommandsInternal`:**
1. After `WarmupBitReader`, create `let mut br = BrotliFastBitReader::new(&s.br, input);`.
2. Non-safe uses of `s.br` become `br`:
   - `CheckInputAmount(safe, &s.br, 28)` becomes `safe || br.avail_in() >= 28`;
   - the `s.br.avail_in` checks in the fast-loop conditions use `br.avail_in()`;
   - `PreloadSymbol` / `ReadPreloadedSymbol` / `ReadSymbol` in the non-safe branches become the `Fast*` versions;
   - the two fast literal loops drop their per-loop `new`/`store`;
   - pass `&mut br` to `ReadCommandInternal` / `ReadDistanceInternal`.
3. Around each non-safe `Decode{Command,Literal,Distance}BlockSwitch` call: `br.store(&mut s.br)` before, `br = BrotliFastBitReader::new(&s.br, input)` after. These are rare.
4. After the loop, `if !safe { br.store(&mut s.br); }`.
   - **Never `store` in safe mode**: `br` is stale there, and storing would clobber `s.br`.
   - `store` adds `start_len - tail.len()` to `next_in`, so nothing else may touch `s.br` between `new` and `store`.

**Risks.**
- Register pressure. An earlier attempt to hoist `br` to the top of the function caused x86-64 spills in the literal loop (-2.5%). A/B on `linux` (x86, `hostlock exclusive` + tuner) and `mac` as well as Android.
- If x86 regresses, gate the persistent reader to `cfg(not(target_pointer_width = "64"))`, since 64-bit refills are rare anyway.
- Expected payoff: most of the `ReadCommand` / `ReadDistance` excess, which I estimate at a third to half of the 32-bit gap. That's an estimate, not a measurement.

**Variant.** `bit_reader` and `decode` are private modules. `BrotliState` has a `pub br` field, though, so external code can read `br.next_in` / `br.avail_in`. Changing `BrotliBitReader` itself to the tail/pointer model everywhere (including the `Safe*` paths) is possible, but it's a bigger and semi-API-visible change.

### 2. Per-command bounds checks

Each costs about 3 instructions per command on ARMv7:
- **`kCmdLut[cmd_code]`** (704 entries): pad the table to 1024 and index `cmd_code & 1023`, or make the command tree's values provably < 704.
- **`dist_context_map[slice_index + distance_context]`**: take a `&[u8; 4]` view per distance block type when it changes, then index it with `context & 3`.
- **`load_huffman_code`** second-level lookups on 32-bit (`table.get(idx)`): they could index the fixed-size `LiteralTree` / `CommandTree` arrays with a masked index if one can be proven safe. The extension index is `value + (bits & mask)`, with `value` up to `u16::MAX`, so this needs care.

### 3. 24-bit refill for `n_bits <= 8` on 32-bit

C refills 3 bytes when `bit_pos <= 8` for small peeks (`PreloadSymbol`, 8-bit `GetBits`). Rust dropped that path in `0466359` / `05ee043` and always refills 16 bits. Restoring it in the fast reader means fewer refills in the literal peek path. Measure; the literal loop is already cheaper than C's, so the gain may be small.

### 4. Copy path

Rust's copy region (`memmove16` plus the post-copy checks, decode.rs around 4020–4060) executes about 2x C's instructions on the wiki q5 profile.
- The 32-bit `memmove16` is a 16-element slice pattern. It compiles to `vld1`/`vst1` with NEON, but check the surrounding checks.
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

### 8. `HuffmanCode` layout

C packs `{bits: u8, value: u16}` into one `uint32_t` load. Rust's `{value: u16, bits: u8}` is two loads (`ldrh` + `ldrb`), at the same instruction count. It's a public-field API change and low payoff, so do it last if at all.

## Not worth retrying

- A local `BrotliBitReader` copy inside the fast literal loops: no change.
- A per-call fast reader (idea 1 without persistence): +17% instructions.
- Making `BitMask(32)` correct on 32-bit (`checked_shr`): +1.4% instructions. It's documented and `debug_assert!`ed as n <= 31 instead.
- From earlier sessions (AGENTS.md log):
  - an unconditional 64-bit refill;
  - `if let` state dispatch;
  - inlining `Decode*BlockSwitch`;
  - hoisting `br` / `state` to the top of `ProcessCommandsInternal` (x86 spills);
  - `#[inline]` on `BrotliStateCleanupAfterMetablock`.
