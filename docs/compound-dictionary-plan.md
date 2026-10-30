# Plan: compound (prepared) raw dictionaries in the Rust encoder

Status: **planned**; decisions recorded under [Decisions](#decisions). Not started. Owner branch: `compound-dict` (see
[Branching](#branching)). Background: findings D2-D6 in
[encoder-parity.md](encoder-parity.md) and
[encoder-parity/dictionary.md](encoder-parity/dictionary.md).

## Goal

Make the Rust encoder handle Compression Dictionary Transport raw
dictionaries the way the C encoder does. The main use case is **multi-megabyte
dictionaries, often larger than the window, used at low qualities**:

1. **Dictionaries larger than the window** are fully addressable for the
   whole stream, independent of `lgwin`, as in C. Today Rust keeps only the
   last `2^lgwin - 16` bytes.
2. **Byte-identical output to C** at every quality where C uses the
   dictionary: q3-q11, lgwin >= 17.
3. **Prepare once, attach many.** Ship `BrotliEncoderPrepareDictionary` /
   `BrotliEncoderAttachPreparedDictionary` / `DestroyPreparedDictionary` (and
   `GetPreparedDictionarySize`) in the FFI. Then a server pays the indexing
   cost once per dictionary, not once per response.
4. **Keep Rust's lower-quality dictionary use where it makes sense.** q2
   should keep using the dictionary (C ignores it there), now through the
   compound index so it also works for dictionaries larger than the window.
5. The decoder needs no work. `brotli-decompressor` >= 6.0.0 already keeps
   dictionaries in their own buffers. It decodes C streams that reference an
   8 MB dictionary with a 256 KB window (verified below).

Non-goals:
- Large-window brotli (lgwin > 24: C's H35/H55/H65).
- Serialized shared dictionaries in the encoder.
- `BROTLI_PARAM_STREAM_OFFSET`.

## Where we are today (measured 2026-10-29)

Static mode. Sizes in bytes. "C" = C reference; "Rust" = current `perf`
branch, which still uses the legacy prefix model and re-hashes the dictionary
on every stream.

| Input / dictionary | lgwin | Q | C | Rust | C, no dictionary |
|---|---:|---:|---:|---:|---:|
| `wiki-Brotli.html` / 8 MB of Wikipedia pages | 22 | 3 | 16730 | 23355 | 42264 |
| same | 22 | 5 | 15621 | 17994 | 37216 |
| same | 22 | 9 | 15419 | 15030 | 36268 |
| same | 18 | 3 | 16730 | **40601** | 42264 |
| same | 18 | 5 | 15621 | **36045** | 37216 |
| same | 18 | 9 | 15419 | **34834** | 36268 |

- C's output doesn't depend on lgwin.
- Rust at lgwin 18 keeps only 256 KB of the dictionary, which is nearly
  useless.
- All 4 encoder/decoder combinations pass in every row. The Rust decoder
  already handles the C streams.

## Target behavior by quality

| Quality | C today | Rust after this plan | Notes |
|---|---|---|---|
| 0-1 | Ignores the dictionary (fast paths) | **Ignore** (same as C and current Rust) | Using it would mean new fast-path designs. q2 is already 1.4-1.7x slower than q1 even without a dictionary (jquery 88 -> 64 MiB/s, wiki 182 -> 107), so silently promoting q0/q1 to q2 would defeat their purpose. Callers who want a dictionary at minimum cost use q2. Possible future work. |
| 2 | Ignores the dictionary (no compound variant of H2). Still shifts static-dictionary distances by the dictionary size. | **Use the compound index, with a shorter chain walk** (Rust extension) | See the prototype numbers below. Output differs from C by design. C's q2 behavior is believed to be a deliberate choice by the brotli team, so this is *not* proposed upstream. |
| 3, 4 | Compound lookup (H3, H4) | Same, **byte-identical** | |
| 4, size hint >= 1 MiB | Ignores the dictionary (H54 has no compound variant; looks like an oversight) | **Use it** (Rust extension), via a compound variant of H54 | Confirmed with the C CLI: a 1.2 MB input gives 180671 bytes with and without `-D`. Upstream issue draft: [upstream/brotli-c-h54-compound-dictionary.md](upstream/brotli-c-h54-compound-dictionary.md). If C fixes it, this becomes parity. Our corpora are < 1 MiB, so the harness doesn't hit this. |
| 5-9 | Compound lookup (H5/H58, H6/H68, H40-42) | Same, **byte-identical** for lgwin >= 17 | At lgwin <= 16, Rust lacks H40-42 (a pre-existing gap); parity there needs those hashers first. |
| 10-11 | Compound lookup in Zopfli (`LookupAllCompoundDictionaryMatches` + `MergeMatches`) | Same, **byte-identical** | |

The static dictionary stays enabled with a custom dictionary, as in C. That
fixes D4.

Two limits to keep from C:
- **Maximum distance:** non-large-window streams can't address beyond
  `BROTLI_MAX_DISTANCE` (0x3FFFFFC, about 64 MiB). The oldest bytes of a
  dictionary larger than about 64 MiB minus the window are unreachable.
- **Dictionary count and size:** up to 15 attached dictionaries
  (`SHARED_BROTLI_MAX_COMPOUND_DICTS`), and `SHARED_BROTLI_MAX_RAW_DICT_SIZE`
  total (2 GiB on 64-bit).

### Prototype: compound dictionary at q2 (C, scratch build)

C was patched to add compound-dictionary variants for H2 and H54, plus a knob
that caps the chain walk in `FindCompoundDictionaryMatch`. Each dictionary is
prepared **once**; the timings are per stream (create, attach, encode,
destroy). Pinned to CPU 6.

| Case | Configuration | Size | MiB/s |
|---|---|---:|---:|
| jquery 3.6.4 -> 3.7.1 (90 KB dictionary) | q1, no dictionary | 35042 | 87.6 |
| | q2, no dictionary | 32435 | 64.1 |
| | **q2 + dictionary, full chain (32)** | **7011** | **73.6** |
| | q2 + dictionary, chain 8 | 7507 | 75.7 |
| | q3 + dictionary (C today) | 7004 | 68.8 |
| | current Rust q2 + dictionary (prefix model) | 11991 | 59.4 |
| `wiki-Brotli.html` / 8 MB dictionary, lgwin 22 | q1, no dictionary | 47355 | 182.4 |
| | q2, no dictionary | 43316 | 107.0 |
| | **q2 + dictionary, full chain** | **16943** | **72.9** |
| | q2 + dictionary, chain 8 | 17960 | 89.7 |
| | q2 + dictionary, chain 2 | 19383 | 100.4 |
| | q3 + dictionary (C today) | 16730 | 66.6 |
| | current Rust q2 + dictionary (prefix model) | 24251 | 28.3 |
| same, lgwin 18 | **q2 + dictionary, full chain** | **16943** | **69.8** |
| | current Rust q2 + dictionary (prefix model) | 41764 | 56.5 |
| `jquery-3.7.1.min.js` / 2.1 MB site-assets dictionary | q2 + dictionary, full chain | 7392 | 55.6 |
| | q2 + dictionary, chain 8 | 8225 | 60.3 |
| | current Rust q2 + dictionary (prefix model) | 18724 | 17.4 |

Takeaways:
- **The compound index at q2 is better than today's Rust q2 everywhere.** It
  is 30-60% smaller, and up to about 3x faster once the prepared
  dictionary is reused, regardless of lgwin.
- **The q2 cost is a dial.** A full chain walk makes q2 about as dense as
  q3, and slightly faster. A short chain (2-8) keeps q2 within 6-16% of its
  no-dictionary speed and still gives most of the gain.
- **Proposed default:** a chain limit of 8 at q2. Decide in phase 7 after
  measuring the Rust implementation.

### Prepared dictionary cost (C)

| Dictionary | Prepare time (one-off) | Prepared index size |
|---|---:|---:|
| 65 KB (wiki-html.dict) | ~1.3 ms | 0.48 MB |
| 90 KB (jquery 3.6.4) | ~3.3 ms | 0.58 MB |
| 2.1 MB (site assets) | ~40 ms | 5.7 MB |
| 8 MB (Wikipedia pages) | ~110 ms | 9.8 MB |
| 17 MB (Wikipedia pages) | - | 20 MB |

- At multi-MB sizes, preparing per stream is prohibitive. Prepare-once is a
  requirement, not an optimization.
- The index is about 1.2-7x the dictionary size, plus the dictionary bytes
  themselves (the lean form references them). Small dictionaries are
  dominated by the fixed bucket table of at least 256 KB.
- The legacy Rust path is equally costly per stream (it re-hashes the
  dictionary every time), which is visible in the table above.

## Design

Mirror C's structures (`compound_dictionary.{c,h}`, `hash.h`) so output
matches by construction.

1. **`PreparedDictionary`** (new module `src/enc/compound_dictionary.rs`).
   - Port of `CreatePreparedDictionaryWithParams`:
     - same parameters: bucket_bits 17-22, slot_bits 7-12, 40-bit hash,
       `kPreparedDictionaryHashMul64Long`, bucket_limit 32, per-slot 0xFFFF
       clamp;
     - same slim layout: `slot_offsets`, u16 `heads`, u32 `items` with the
       chain-end bit.
   - Storage is allocator-backed (`Allocator<u16/u32>` are already in
     `BrotliAlloc`), so it stays `no_std`.
   - Source bytes are either owned or borrowed. Follow the decoder's
     `MaybeOwnedSlice` precedent (`rust-brotli-decompressor/src/state.rs`):
     `&'static` is forged only at the FFI boundary.
2. **`CompoundDictionary`** on the encoder state, *not* in
   `BrotliEncoderParams` (params is cloned per meta-block).
   - Fields: up to 15 chunks, `chunk_offsets`, `total_size`.
   - Attach is allowed only before the first input, as in the decoder. C
     doesn't check this for raw dictionaries; Rust should reject late
     attaches.
   - A shared prepared dictionary must be usable by many encoders at once.
     The state holds a borrowed view, not a copy.
3. **Distance plumbing.**
   - Pass `gap = total_size` where Rust currently hard-codes 0:
     `CreateBackwardReferences` `mod.rs:2408`; `hq.rs:673, 898, 1283`;
     `ZopfliCreateCommands` `hq.rs:114`.
   - The distance code at commit time must use `max_distance + gap`
     (`mod.rs:2512-2513`).
   - `FindLongestMatch` already receives `gap` for static-dictionary
     distances.
4. **Greedy lookups.**
   - Port `FindCompoundDictionaryMatch` / `LookupCompoundDictionaryMatch`.
     Keep its exact 4-byte pre-compare and `len >= 4` acceptance.
   - Call it after `FindLongestMatch` in the main and lazy loops
     (`mod.rs:2440-2453`, `:2475-2488`).
   - Rust gates on `FindLongestMatch`'s `bool` return, so the compound hit
     must be folded into it, or the code must switch to C's
     `score > kMinScore` test.
   - Gate by C's hasher choice for parity: (quality, size_hint, lgwin) ->
     {3, 4, 5, 6, 40-42}, plus the Rust extensions {2, 54}.
5. **Zopfli (q10/q11).**
   - Port `FindAllCompoundDictionaryMatches`, `LookupAll...` and
     `MergeMatches`.
   - Grow the q10 match buffer (`hq.rs:888`) and set
     `lz_matches_offset`/`shadow_matches` to C's values when a dictionary is
     attached.
   - Add `UpdateNodes`' compound last-distance branch (C
     `backward_references_hq.c:470-510`).
6. **`ExtendLastCommand` compound branch** (C `encode.c:942-975`).
7. **Remove the prefix model from the compound path:**
   - no ring-buffer prepend;
   - no `last_*_pos_ = dict_size`;
   - no `ringbuffer_break` / `fix_unbroken_len` needed;
   - static dictionary stays on.

   A dictionary match then can't cross into the input by construction (C
   clamps matches to the chunk end). That removes the bug class behind D5.
8. **Public API.**
   - **FFI:** `BrotliEncoderPrepareDictionary(type, size, data, quality,
     alloc, free, opaque)` (RAW only; quality ignored, as in C),
     `BrotliEncoderDestroyPreparedDictionary`,
     `BrotliEncoderAttachPreparedDictionary`,
     `BrotliEncoderGetPreparedDictionarySize`. Add them to
     `src/ffi/compressor.rs`, re-export via `c/src/lib.rs`, and add dummy
     calls in `instantiate_functions`.
   - **Rust:** `PreparedDictionary::new(...)` plus
     `BrotliEncoderStateStruct::attach_prepared_dictionary(...)`.
   - **`BrotliEncoderSetCustomDictionary` /
     `set_custom_dictionary`:** re-implement on the compound model (prepare
     internally, owned). Existing callers then get dictionaries larger than
     the window and C-like output automatically. Their output changes, which
     is intended.
   - Keep `set_custom_dictionary_with_optional_precomputed_hasher` (used by
     the multithreaded catable path) on the legacy prefix model unless review
     says otherwise.

## Phases (one or more commits each; every commit passes the gates)

1. **Harness.**
   - `--lgwin` is already added to `brotli_compare` / `bench_*`.
   - Add large-dictionary cases through a fetch script and an opt-in
     `--set large` (Decision 4). Include multi-MB dictionaries, and
     dictionary-larger-than-window runs at `--lgwin 18`.
   - Cache the C prepared dictionary per (dictionary, quality) cell instead
     of re-preparing per stream, and report prepare time separately.
   - Add a Rust prepared-dictionary binding in `StandardBrotliApi`, falling
     back to `SetCustomDictionary` until phase 6 lands.
2. **`PreparedDictionary` port**, with a unit test that compares the index
   arrays against C for a few dictionaries. Use a small C test helper linked
   in the bench harness.
3. **Compound state, attach, `gap` plumbing, greedy lookups for H3/H4/H5/H6**
   (and H9 where Rust still uses it). Target: dictionary cases byte-identical
   to C at q3-q9, lgwin >= 17.
4. **`ExtendLastCommand` compound branch.**
5. **Zopfli q10/q11 compound matches.** Target: byte-identical at q10/q11.
6. **FFI prepared-dictionary API**, and re-route `SetCustomDictionary`.
   Switch the harness to prepare once for both implementations, so the
   dictionary encode benchmark becomes like-for-like.
7. **Rust extensions:** a compound variant for H2 (q2) with a chain limit,
   and for H54 (q4 with size hint >= 1 MiB). Pick the q2 chain limit from
   measurements (proposed: 8). Document as intentional C divergences in
   encoder-parity.md.
8. **Docs:**
   - encoder-parity.md: mark D2/D3/D4/D6 resolved or replaced by the
     intentional divergences;
   - development.md: update the gates;
   - baseline.md: add a new dated dictionary measurement section;
   - AGENTS.md: update.

## Verification

- **Exact-match targets (`--verify`):**
  - dictionary cases bit-exact with C at q0/q1 and q3-q11, at lgwin 18, 20
    and 22;
  - q2 (and H54 configurations) excluded as intentional divergences;
  - standard cases stay 204/204.
- **Decode parity** stays 100%. Add C-encoded and Rust-encoded
  dictionary-larger-than-window streams decoded by both decoders.
- **Fuzzing:** extend the scratch dictionary fuzzer (random dictionary and
  input pairs, including dictionaries larger than the window, several
  chunks, q2-q11, static and flushed). Require 0 undecodable streams and
  exact C parity where expected.
- `cargo test --release --lib`, plus `no_std` and `simd` builds.
- **Perf:**
  - A/B against the parent commit at q2-q11, with and without a dictionary.
    Non-dictionary encodes must not regress beyond noise.
  - Track prepare time and index memory against C.

## Decisions

Recorded 2026-10-30.

1. **Ownership: borrowed ("lean") for the FFI, matching C exactly.**
   - `BrotliEncoderPrepareDictionary` returns an opaque, immutable handle.
     It owns only the index (allocated with the caller's `alloc_func` /
     `free_func`) and *references* the caller's dictionary bytes. The caller
     keeps the bytes alive until `BrotliEncoderDestroyPreparedDictionary`.
   - `BrotliEncoderAttachPreparedDictionary` stores a pointer. The prepared
     dictionary must outlive every encoder it is attached to, and it can be
     shared by any number of encoders on any number of threads (read-only
     after creation).
   - This is C's contract, so a C caller can switch libraries without code
     or memory-model changes, and nothing multi-MB is copied per encoder.
   - Inside Rust, `&'static` is forged only at the FFI boundary, as the
     decoder does in `BrotliDecoderAttachDictionary`.
   - The safe Rust API gets an owned-or-borrowed wrapper (decoder's
     `MaybeOwnedSlice` precedent). It doesn't affect C callers and can be
     refined during review.
2. **Re-route `BrotliEncoderSetCustomDictionary` / `set_custom_dictionary`**
   to the compound model. The output change is accepted and will be called
   out in the upstream PR and release notes.
3. **Upstream (google/brotli):**
   - Report the H54 gap (draft above; H35 included as the same omission).
   - q2 is a deliberate C choice: Rust's q2 dictionary use stays a
     documented Rust-only divergence.
4. **Large-dictionary test data comes from a fetch script only; nothing is
   committed.** Phase 1:
   - add a script (e.g. `scripts/fetch_large_dicts.sh`) that downloads and
     assembles multi-MB dictionaries and inputs into a gitignored
     `corpora/large/`, plus a generated `corpora/large/manifest.tsv`;
   - give the harness an opt-in case set (e.g. `--set large`) that loads
     that manifest when present and errors with a hint to run the script
     when it's not;
   - default `--verify` stays self-contained.

## Risks and open questions
- **Rust hasher gaps** (no H40-42 at lgwin <= 16; H6 thresholds differ for
  inputs >= 1 MiB) limit exact parity in those configurations. That is
  independent of this work.
- **Legacy `SetCustomDictionary` output changes** for existing Rust users.
  It's a behavior change, but it fixes truncation; call it out in the upstream
  PR and release notes.
- **Multithreaded / catable path** keeps the prefix model; confirm it's
  unaffected.
- **Upstream:**
  - The H54 gap is to be reported to google/brotli (Decision 3).
  - The Rust work goes to dropbox/rust-brotli as a series. Prepared-dictionary
    struct and attach first, then lookups by quality tier, then FFI.

## Branching

A new branch `compound-dict` in `vendor/rust-brotli`, stacked after `parity`
and before `perf`:

```
master -> fix-dict-span -> parity -> compound-dict -> perf
```

`perf` is still empty, so the rebase is trivial. Perf work that lands
first should be rebased on top. Update the
[development.md](development.md) status table when the branch is created.
