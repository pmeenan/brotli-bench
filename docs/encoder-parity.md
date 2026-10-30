# Encoder parity: where and why Rust output differs from C

The Rust encoder (`vendor/rust-brotli`, `6df909c`) is a port of an older
version of the C encoder. The C reference here is `vendor/brotli-c` at
`d89c4b1` (v1.2.0+195). Every Rust stream decodes correctly, but at most
qualities the bytes are not the same as C's. This document lists every
divergence found in the 2026-10-29 baseline, explains why it happens, and
points to the fix.

Baseline parity, from [baseline/verify-2026-10-29.csv](baseline/verify-2026-10-29.csv).
Static and flushed modes have the same counts.

| Quality | Standard cases (17) bit-exact | Dictionary cases (8) bit-exact |
|---:|---:|---:|
| 0-1 | 17/17 | 0/8 |
| 2-4 | 1/17 (`monkey` only) | 0/8 |
| 5 | 14/17 | 0/8 |
| 6-8 | 15/17 | 0/8 |
| 9 | 2/17 | 0/8 |
| 10-11 | 1/17 | 0/8 |

The investigation was split into four parts, each written up in detail with
file:line references for both codebases:

- [encoder-parity/build-flags.md](encoder-parity/build-flags.md): is either
  output a function of compiler flags? **No.**
- [encoder-parity/q2-q4.md](encoder-parity/q2-q4.md): the "quickly" hashers
  and the shared backward-reference loop.
- [encoder-parity/q5-q11.md](encoder-parity/q5-q11.md): hasher choice, Zopfli,
  float precision, block splitting.
- [encoder-parity/dictionary.md](encoder-parity/dictionary.md): raw
  (Compression Dictionary Transport) dictionaries.

Every cause below was **confirmed experimentally**: patching a scratch copy of
rust-brotli made the affected outputs byte-identical to C. Items marked
*suspected* were found by code reading but are not exercised by the corpora.

## Catalog of divergences

Origin key:
- **C change** means C changed after the Rust port and Rust was never updated.
- **Port bug** means Rust deviates from the C it was ported from.
- **Design** means the two use intentionally different approaches.

| ID | Divergence | Affects | Origin | Fix size |
|---|---|---|---|---|
| E1 | Missing RLE "hash poisoning" guard after each copy in `CreateBackwardReferences` | q2-q9; all q5-q8 diffs | C change (a629289, #590, 2017) | ~6 lines |
| E2 | Old 64-bit hash multiplier in H2/H3/H4/H54 `HashBytes` | q2-q4 | C change (117b68b, 2023) | 4 lines |
| E3 | H2 takes a bucket match even when its score is below `kMinScore` (C falls back to the static dictionary) | q2 | Port bug | few lines |
| E4 | Bucket-sweep slot layout: adjacent slots instead of slots 8 apart and wrapped | q3-q4 | C change (#749, 2019) | ~5 sites |
| E5 | Literal-spree skip near the end of a block: Rust emits literals, C keeps searching | q2-q9, rare (seen only with H54) | Port bug | small |
| E6 | q9 uses a Rust-only `H9` hasher; C uses H5 with `block_bits=8` | q9 | Rust-only | remove a branch |
| E7 | `FastLog2f64` computes log2 in f32 (C uses double for v >= 256) in literal-cost estimation | q10-q11 commands | Port bug | small |
| E8 | Bit-cost / population-cost / clustering / block-splitter math in f32 (C uses double). Also 3 instead of 10 block-split iterations at q11 | q10-q11 headers | Port bug | large (type plumbing) |
| E9 | Off-by-one in `StitchToPreviousBlockH10` (missing `+ 1`) | q10-q11 flushed | Port bug | 1 char |
| D1 | Setting a dictionary forces `catable`/`appendable` even at q0/q1, where it is then ignored | dictionary q0-q1 | Port quirk | 2 lines |
| D2 | C never searches the dictionary at q2 (H2 has no compound-dictionary variant); Rust does | dictionary q2 | Design | keep (Rust is much smaller) |
| D3 | C uses a separate compound-dictionary index; Rust prepends the dictionary to the window and hashes it as history | dictionary q3-q11 | Design | port `PreparedDictionary` (large) |
| D4 | Rust disables the static dictionary when a custom dictionary is set | dictionary q2-q11 | Port quirk | 1 line |
| D5 | **Correctness bug:** at q10/q11 Rust can emit copies that start in the dictionary and run into the input; both decoders reject these streams | dictionary q10-q11 | Port bug | 1 line |
| D6 | Window reach: Rust's dictionary lives inside the window. It is tail-truncated to `2^lgwin - 16` and scrolls out once `dict_size + pos` exceeds the window. C's compound dictionary stays addressable for the whole stream. | dictionary, large responses (not hit by the corpora at lgwin 22) | Design (part of D3) | comes with D3 |

Latent or suspected issues that the corpora don't exercise:

- `FastLog2u16(p as u16)` in `bit_cost.rs` truncates counts >= 65536. This
  gives wrong costs for large histograms.
- Hasher thresholds for inputs >= 1 MiB differ. Rust's H6 condition and H5
  `bucket_bits` size-hint checks are not C's
  (`src/enc/encode.rs:863,880` vs `c/enc/quality.h:175-209`). A 6.5 MB input
  still differs at q5-q8.
- AdvHasher `StoreRangeOpt` stores a masked position. That only matters past
  the ring-buffer size.
- The quickly hasher's last-distance probe lacks C's
  `cached_backward <= max_backward` check. It can't trigger at lgwin 22.
- **Small windows (lgwin <= 16):** C uses H40/H41/H42 at q5-q9. Rust has those
  hashers commented out and falls back to H6, so q5-q9 all give the same
  output (alice29: Rust 52110 bytes at every quality; C 53089 / 52384 /
  52068 / 51790 / 51695). The harness only tests lgwin 22.
- The `float64` cargo feature does not compile. The `portable-float` feature
  changes output and *lowers* C parity (85/204 vs 100/204), so it should stay
  off.

Ruled out (see [build-flags.md](encoder-parity/build-flags.md)):

- C output is identical across `-O0/-O2/-O3`, `-march` variants,
  `-ffp-contract=off`, clang, and C's SIMD hashers (H58 is used at q5-q6 on
  gcc/Linux and gives the same output as H5).
- Rust output doesn't change with `target-cpu` or LTO.
- FMA contraction is not a factor.
- Only `-ffast-math` changes C output (at q10/q11).

### Not an encoder issue, but noticed

- **Decoder error strings differ.** C's `BrotliDecoderErrorString` returns
  names with a leading underscore (e.g. `_ERROR_COMPOUND_DICTIONARY`), while
  the Rust FFI returns `ERROR_COMPOUND_DICTIONARY`. The error codes match.

## Combined fix series

[encoder-parity/patches/](encoder-parity/patches/) has an 8-commit
`git format-patch` series against rust-brotli `6df909c`, one commit per fix:

| Patch | Fixes |
|---|---|
| 0001 | E1: RLE hash-poisoning guard |
| 0002 | E2-E5: quickly hashers (multiplier, H2 `kMinScore`, sweep layout, end-of-block search) |
| 0003 | E6: H5 at q9. H9 is kept for `q9_5` and for lgwin <= 16, where Rust lacks C's H42. |
| 0004 | E7: `FastLog2f64` returns f64. no_std / `portable-float` keep the table. |
| 0005 | E8: f64 bit-cost, clustering and block-split math; 10 split iterations at q11. no_std-safe; adds an `Allocator<u64>` bound to `BrotliSplitBlock`. Replaces the 8xf32 SIMD `FindBlocks` cost update (`update_cost_and_signal`, `Mem256f`) with a scalar f64 loop. |
| 0006 | E9: `StitchToPreviousBlockH10` `+ 1` |
| 0007 | D1: no forced `catable`/`appendable` at q0/q1 |
| 0008 | D5: clamp `FindAllMatchesH10` short-range matches at the dictionary boundary |

Results with the series applied to a scratch copy, everything else
unchanged. From the full `--verify`, 600 configurations:

| Q | Standard static | Standard flushed | Dictionary static | Dictionary flushed |
|---:|---:|---:|---:|---:|
| 0-1 | 17 -> 17 | 17 -> 17 | 0 -> **8** | 0 -> **8** |
| 2-4 | 1 -> **17** | 1 -> **17** | 0 -> 0 | 0 -> 0 |
| 5 | 14 -> **17** | 14 -> **17** | 0 -> 0 | 0 -> 0 |
| 6-8 | 15 -> **17** | 15 -> **17** | 0 -> 0 | 0 -> 0 |
| 9 | 2 -> **17** | 2 -> **17** | 0 -> 0 | 0 -> 0 |
| 10-11 | 1 -> **17** | 1 -> **17** | 0 -> 0 | 0 -> 0 |

- **Standard cases: 204/204 bit-exact in both modes.** Overall exact streams
  go from 200/600 to 440/600. Decode parity stays 600/600.
- **Dictionary cases** become exact at q0/q1. At q2-q11 they stay different
  because of D2/D3, which the series does not touch. D4 is also not included
  because re-enabling the static dictionary is part of the D3 decision.
- **The span repro** passes at every quality and is bit-exact with C at
  q10/q11.
- **Tests and builds:** `cargo test --release --lib` passes 43/43.
  `--no-default-features` and `--features simd` build cleanly.
- **Size change of Rust output:** -0.03% to +0.02% on the standard set. Up to
  +0.24% on dictionary q2-q4, where the corrected hashers make the
  prefix-history model slightly worse.
- **Speed: parity costs Rust encode speed.** This is the geomean over the
  default bench subset, static mode, measured on the same core back-to-back
  with the unpatched build (two runs; C drift between runs was <= 3%):

  | Q | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
  | Rust encode, standard | -1% | +1% | -6..-10% | -6..-7% | -3..-5% | -2% | -9% | +6..+9% | -11..-12% | -13..-16% | -7% | -11% |

  Likely causes, not yet bisected:
  - **q2-q4:** the C sweep layout spans two cache lines.
  - **q9:** H5 with `block_bits=8` searches more than H9.
  - **q10/q11:** a scalar f64 `FindBlocks` loop replaces the 8xf32 SIMD
    version, plus 10 split iterations at q11.

  - **q6/q8 slower, q7 faster:** no fix is specific to these qualities, so
    this may be codegen/LTO noise. A per-commit bisect is needed.

  These costs are real C behavior that the port skipped. They are good first
  optimization targets once parity is locked in. Decode speed is unchanged.

The patches have not been applied to `vendor/rust-brotli`. That decision is
left to the reader.

## Recommendations

> Encoder work is tabled while decoder performance is the priority
> ([decoder-perf-plan.md](decoder-perf-plan.md)). Item 1 is done and in
> upstream review as dropbox/rust-brotli#278.


1. **Fix D5 first.** It is a correctness bug: undecodable output in a
   Chrome-relevant mode. The repro is in
   [encoder-parity/repro/dict-span/](encoder-parity/repro/dict-span/):
   ```bash
   ./build/brotli_compare --verify --quality 11 \
     --corpus docs/encoder-parity/repro/dict-span/input.txt \
     --dictionary docs/encoder-parity/repro/dict-span/dict.bin   # RC/RR fail today
   ```
2. **Land the parity fixes before optimizing.** Then bit-exact output becomes
   a cheap, strict regression test for performance work:
   - E1-E7 and E9 are small;
   - E8 is the only invasive one;
   - D1 and D4 are one or two lines each.

   With these, any change to Rust output during optimization is
   immediately visible as a bitstream diff instead of a size wobble.
3. **The dictionary model (D3) is a product decision.** Decided: port C's
   model, keep q2 dictionary use as a Rust extension. See
   [compound-dictionary-plan.md](compound-dictionary-plan.md). Porting C's
   compound/prepared dictionary gives C parity and much better q3-q6 delta
   compression (up to 65% smaller). It also allows prepared-dictionary reuse
   through the FFI (`BrotliEncoderPrepareDictionary` /
   `BrotliEncoderAttachPreparedDictionary`). The cost is that Rust's current
   slight win at q7-q9 on JS deltas goes away. Keep D2 (Rust's q2 dictionary
   use) unless strict parity is required.
4. Keep `portable-float` off for parity work. Treat E8's f32-vs-double split
   as "match C exactly": the Zopfli cost model is `float` in C too, so it stays
   f32.

## Tools

- `./build/brotli_compare --verify --verify-csv FILE --dump-dir DIR` records
  per-configuration parity and writes both streams for every non-identical
  encode.
- [`scripts/brstruct.py`](../scripts/brstruct.py)` STREAM.br ORIGINAL` prints
  a diffable structural dump: meta-block headers, block switches, context
  maps, prefix-code hashes, and one line per command. To find the first
  divergence:
  ```bash
  ./build/brotli_compare --verify --corpus alice29.txt --quality 5 --mode static --dump-dir /tmp/d
  diff <(scripts/brstruct.py /tmp/d/alice29.txt.static.q5.c.br vendor/brotli-c/tests/testdata/alice29.txt) \
       <(scripts/brstruct.py /tmp/d/alice29.txt.static.q5.rust.br vendor/brotli-c/tests/testdata/alice29.txt)
  ```
  Filter with `grep -E '^   C[0-9]'` to compare only commands, which separates
  match-finding differences from block-split and clustering differences.
- **`vendor/brotli-c/research/brotlidump.py` has two bugs** that mislead
  multi-meta-block diffs:
  - it resets the distance ring and literal context at each meta-block;
  - it decodes NDIRECT direct distances as `dcode-16` instead of `dcode-15`.

  `brstruct.py` avoids it.
- **Scratch tools that were not committed.** The investigation also used:
  - a `BROTLI_ENABLE_LOG` command dumper;
  - a dictionary-aware patched `brotlidump.py`;
  - a C "prefix emulation" driver (dictionary fed as history, then flush,
    then input);
  - a `BrotliEncoderPrepareDictionary` timer;
  - a random dictionary/input fuzzer (it found D5).

  Ask if any of these should be added to `scripts/`.
