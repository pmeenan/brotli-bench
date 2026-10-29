## Raw-dictionary (Compression Dictionary Transport) divergences

Scope: all 8 cases in `corpora/dictionary/manifest.tsv`, q0-q11, static and
flushed (0/48 exact per mode in `docs/baseline/verify-2026-10-29.csv`).
Harness: C = `BrotliEncoderPrepareDictionary(RAW, quality)` +
`BrotliEncoderAttachPreparedDictionary`; Rust = legacy
`BrotliEncoderSetCustomDictionary` (`src/streaming_harness.h:55-76`).
Machine: 2x24-core Xeon @2.00GHz (GCE), experiments pinned to CPUs 30-38.
Tools: a dictionary-aware command dumper (a patched copy of
`vendor/brotli-c/research/brotlidump.py`), encoder drivers, a decode checker,
a C "prefix emulation" driver and a fuzzer. These were scratch tools and are
not committed. The span repro is in [repro/dict-span/](repro/dict-span/).

**Summary.** The two encoders use different dictionary models, so they are
expected to differ:

* **C (compound dictionary):** the dictionary stays *outside* the ring buffer.
  It is indexed once by `CreatePreparedDictionary` (a separate hash index that
  doesn't depend on quality). At q3+ each position probes that index in
  addition to the normal hasher.
* **Rust (prefix history):** the dictionary is copied *into* the ring buffer
  as history before position 0 and inserted into the normal hasher.

Both models produce the same distance arithmetic in the decoder (as long as
dict + input < window), so the streams cross-decode. The search strategy,
however, is completely different. On top of the model difference there are
three Rust-side issues:

* a q0/q1 `catable` side effect, which is the only reason q0/q1 differ (fixed
  and confirmed bit-exact);
* the static dictionary is disabled when a custom dictionary is set;
* **a correctness bug at q10/q11:** Rust can emit copies that run from the
  dictionary into the input. Both the C and Rust decoders reject these
  streams. Fixed and confirmed.

### D1. q0/q1: Rust switches to "catable" mode when a dictionary is set (CONFIRMED)

1. **Affects:** every dictionary case at q0 and q1, static and flushed. C's
   output with a dictionary is byte-identical to its output without one: the
   q0/q1 fast paths never look at the dictionary.
2. **First difference:** stream byte 0 (`first_diff=0`). After WBITS, Rust
   writes a 2-byte **uncompressed meta-block** (the first 2 input bytes),
   then the compressed data, then a separate empty ISLAST meta-block. C
   writes one compressed meta-block. Example: `brcmds.py` on the jquery q0
   streams gives C `MLEN=87533`, Rust `UNCMPR MLEN=2` + `MLEN=87531`.
3. **Root cause:**
   * In Rust `src/enc/encode.rs:1237-1241`, `set_custom_dictionary_with_optional_precomputed_hasher`
     ignores the dictionary at q0/q1 / size <= 1, but first sets
     `params.catable = true; params.appendable = true;` ("don't risk a
     too-short dictionary").
   * `catable` then has these effects:
     * It disables the fast path (`encode.rs:2930`,
       `(quality == 0 || quality == 1) && !catable` -> `compress_stream_fast`).
       Rust therefore runs q0/q1 through `encode_data` with whole-window blocks.
     * It forces the 2-byte uncompressed meta-block (`encode.rs:2288-2350`).
     * `appendable` turns off `is_last` and adds the trailing empty
       meta-block (`encode.rs:1976-1979`, `1931-1943`).
   * Because `ensure_initialized` already ran (`encode.rs:1234`),
     `SanitizeParams`/dist-cache poisoning (`encode.rs:559-566`, `693-703`)
     never see these flags.
   * C has no equivalent: `BrotliEncoderPrepareDictionary` ignores
     `quality` for RAW (`c/enc/encode.c:1805`), and the fast path
     (`BrotliEncoderCompressStreamFast`, `c/enc/encode.c:1437-1559`) never
     reads `params.dictionary`.
4. **Evidence:**
   * In a scratch copy of rust-brotli, I gated those two
     lines behind an env var and rebuilt the FFI (same `rust/Cargo.toml`
     profile). Rust then matches C **bit-exactly on 32/32** (8 cases x
     q0,q1 x static,flushed), and every stream C-decodes with the dictionary.
   * With the env var set, the patched build reproduces the original
     streams byte-for-byte.
5. **Fix:** delete `encode.rs:1238-1239` (just `return`).
   * Compression: neutral. Rust's output becomes C's, and the old Rust
     output was -114..+77 B off it.
   * Speed: Rust q1 wiki-Brotli.html + 64 KB dictionary goes from 91.4 to
     110.1 MiB/s, the same as without a dictionary (111.5).
     (`bench_rustx --op encode --quality 1`, 300 iterations, CPU 38.)

### D2. q2: C ignores the dictionary for matching; Rust uses it (CONFIRMED, by design)

1. **Affects:** all cases at q2. Rust is far smaller (jquery 11824 vs C
   32435; bootstrap 712 vs 36557). C with a dictionary is 0-15 B *larger*
   than C without one (e.g. wiki-Brotli 43331 vs 43316).
2. **First difference:** in the command stream, at the first
   dictionary-backed copy (Rust's are `D` copies; C has none).
3. **Root cause:**
   * C only searches the compound dictionary in the `CreateBackwardReferencesDH*`
     variants, which exist for hashers 3/4/5/6/40/41/42/55/65 (+58/59/68
     SIMD) (`c/enc/backward_references.c:295-322`). q2 uses H2
     (`c/enc/quality.h:181-182`), so it hits `default:` and falls through to
     the no-dictionary `NH2` path.
   * `gap = params->dictionary.compound.total_size` is still applied
     (`c/enc/backward_references_inc.h:31,197-199`), so **static-dictionary
     distances are shifted by `dict_size`**. That shift costs the extra bytes.
     Confirmed: the C q2 wiki-Brotli command streams with and without the
     dictionary are identical except that all 21 static-dictionary refs have
     distance +65256 (e.g. 13435 -> 78691).
   * Rust has no quality gate. `HasherPrependCustomDictionary`
     (`src/enc/encode.rs:1163-1194`, called from `1257`) loads the dictionary
     into H2, and H2 finds it like any other history.
4. **Fix (only if strict parity is wanted):** at q2, skip the prepend but
   keep the ring-buffer offset (static-dictionary distances must still
   include `dict_size`). This is a large compression regression and not
   recommended; better to document it as a Rust advantage. Note: at q0/q1 a
   server may skip attaching the dictionary altogether (no static refs, so
   the output is identical). At q2 it may not: a dcb decoder with the
   dictionary attached would misread the unshifted static-dictionary
   distances.

### D3. q3-q11: compound index vs dictionary-as-history (CONFIRMED as the dominant cause)

1. **Affects:** all cases q3-q11.
   * At q3-q6 Rust is much worse: jquery q3 10987 vs 7004 (+57%), react
     4855 vs 3061, bootstrap 649 vs 224.
   * At q7-q9 Rust is slightly better on the JS/CSS deltas: jquery q9 6496
     vs 7029.
   * Rust stays slightly worse on wiki.
   * At q10/q11 the two are within about 2%.
2. **First difference:** the command stream from the first copy onwards (the
   `first_diff` byte is in the first meta-block's prefix codes, offset
   3-34). Command totals for jquery (`cmdstats.py`, D = dictionary copies,
   W = window copies):

   | q | C lits / D bytes / W bytes | Rust lits / D bytes / W bytes |
   |---|---|---|
   | 3 | 2047 / 76839 / 8647 | 3063 / 65455 / 19015 |
   | 5 | 1750 / 75138 / 10624 | 1847 / 72153 / 13533 |
   | 7 | 1718 / 74707 / 11092 | 1713 / 75034 / 10786 |
   | 9 | 1716 / 74656 / 11145 | 1664 / 76037 / 9832 |

   C's dictionary coverage is flat from q3 to q9. Rust's grows with quality.

3. **Root cause (the models):**
   * **C:**
     * `CreatePreparedDictionary` (`c/enc/compound_dictionary.c:151-169`,
       `13-149`) builds a quality-independent index: 5-byte hash
       (`hash_bits=40`), >= 2^17 buckets, up to 32 entries per bucket
       (`bucket_limit=32`, trimmed per slot so offsets fit in 16 bits).
     * At every position, `LookupCompoundDictionaryMatch` ->
       `FindCompoundDictionaryMatch` (`c/enc/hash.h:537-644`, `717-732`) runs
       *in addition to* the ring-buffer hasher. It checks the 4 cached
       distances, then walks the whole chain (len >= 4, limited to the end
       of the dictionary) (`c/enc/backward_references_inc.h:200-204,
       231-236`). Zopfli uses `LookupAllCompoundDictionaryMatches`
       (`c/enc/backward_references_hq.c:433-497,718-721`).
     * As a result, C finds dictionary matches about equally well at q3 and
       at q9, regardless of how weak the quality's own hasher is (H3 = 2-way
       sweep at q3).
   * **Rust:**
     * The dictionary goes into the ring buffer (`src/enc/encode.rs:1248-1250`;
       `last_processed_pos_ = dict_size`) and every dictionary position is
       inserted into the *same* hasher as the input (`StoreLookaheadThenStore`,
       `encode.rs:1181-1192`). Hasher choice: `encode.rs:834-880`.
     * At q3/q4 (H3: 2^16 buckets x sweep 2; H4: 2^17 x sweep 4), about
       90-230 K dictionary positions compete for the same slots as the input,
       and most get evicted. At q7-q9 (H5 with 2^(q-1) entries per bucket, H9
       at q9), the deeper chains search the dictionary *better* than C's
       fixed 32-entry index. They also find 4-byte and cross-chain matches,
       hence Rust's small win.
   * **Clinching evidence (C with Rust semantics):** `prefix_emul` feeds the
     dictionary through the *C* encoder, FLUSHes, then compresses the input
     in the same stream. The full stream verifies (decodes to dict+input).
     The bytes emitted after the flush nearly reproduce Rust's sizes, with
     the same q3-q6 penalty and the same q7-q9 advantage. Cells are C /
     Rust / C-emulating-Rust, static:

     | case | q3 | q4 | q5 | q7 | q9 |
     |---|---|---|---|---|---|
     | jquery | 7004 / 10987 / 10835 | 7034 / 9597 / 9607 | 6981 / 7873 / 7870 | 7024 / 6986 / 6980 | 7029 / 6496 / 6485 |
     | react-dom | 3061 / 4855 / 4876 | 3046 / 4081 / 4339 | 3048 / 3091 / 3090 | 3053 / 3008 / 3008 | 3053 / 3005 / 3005 |
     | lodash | 7041 / 10435 / 10512 | 6974 / 9739 / 9814 | 6954 / 7014 / 7010 | 6965 / 7009 / 7000 | 6972 / 6993 / 6989 |
     | vue | 34060 / 43794 / 43981 | 33765 / 40458 / 40680 | 33210 / 35663 / 35659 | 33075 / 33146 / 33142 | 33013 / 32144 / 32144 |
     | bootstrap | 224 / 649 / 616 | 214 / 573 / 425 | 193 / 348 / 347 | 188 / 198 / 197 | 188 / 192 / 191 |
     | wiki-Brotli | 27729 / 29909 / 29911 | 27079 / 28675 / 28452 | 25107 / 25741 / 25526 | 24833 / 25131 / 24915 | 24618 / 24698 / 24531 |
     | wiki-Huffman | 39588 / 42107 / 42127 | 38187 / 40138 / 39754 | 35659 / 36617 / 36291 | 35083 / 35533 / 35233 | 34874 / 35090 / 34756 |
     | wiki-Espresso | 62072 / 65564 / 65778 | 60023 / 62434 / 62347 | 55768 / 57140 / 56648 | 54791 / 55342 / 55055 | 54320 / 54555 / 54264 |

     The remaining gap between Rust and the emulation (mostly on wiki, up to
     about 2%) comes from D4 plus the ordinary non-dictionary divergences
     (see the q2-q4 / q5-q11 sections).

4. **Other semantic differences of the prefix model** (suspected minor for
   these corpora unless noted):
   * **Rust disables the static dictionary.** `encode.rs:1213` sets
     `params.use_dictionary = false`, which gates static-dictionary search
     in `src/enc/backward_references/mod.rs:2581ff`. C keeps it (with the
     `gap` shift). On wiki-Brotli, C emits 198 static refs (1620 B) at q5 and
     182 at q9; Rust emits none. This is the main reason Rust loses slightly
     on wiki at q7-q9 even though it wins on the JS deltas. Fix: drop the
     assignment (the distances are already correct because `max_distance`
     includes `dict_size`, matching the decoder's
     `distance - max_distance - 1 - compound_size`, `c/dec/decode.c:2213-2242`).
   * **Window reach.** In Rust the dictionary takes up window space and
     scrolls out once `dict_size + pos > 2^lgwin - 16` (it is also
     tail-truncated to `2^lgwin - 16`, `encode.rs:1231-1246`). C's compound
     dictionary stays addressable just past `max_distance` for the whole
     stream (`c/dec/decode.c:2213`). This doesn't matter for the corpora
     (inputs <= 458 KB, lgwin 22) but does matter for large responses.
   * **Literal context / ring-buffer bytes before position 0.** Both
     bitstreams start with p1 = p2 = 0, and Rust resets `prev_byte_`/`prev_byte2_`
     (`encode.rs:1215-1224`). Rust heuristics that read `data[pos-1]`, however,
     see the dictionary tail where C sees zeros. This only affects the first
     bytes.
   * **Distance cache.** Both start from {4,11,15,16}. C tests cached
     distances against the dictionary explicitly (`hash.h:578-603`); in
     Rust the ring-buffer hasher covers this implicitly.
   * **`ExtendLastCommand`.** C has a compound branch that extends a copy
     into the dictionary across `EncodeData` calls
     (`c/enc/encode.c:942-960`); Rust's version (`encode.rs:360ff`) only
     sees the ring buffer, which contains the dictionary. The two are
     equivalent while dict + input < window.
5. **Fix / recommendation:** to get parity and C-like q3-q6 compression,
   port the compound-dictionary model:
   * `PreparedDictionary` / `CreatePreparedDictionary`;
   * `FindCompoundDictionaryMatch` in the greedy backward-reference loop
     (the "DH" variants) and `LookupAllCompoundDictionaryMatches` in
     Zopfli;
   * `gap` handling;
   * the `ExtendLastCommand` compound branch;
   * `BrotliEncoderPrepareDictionary` / `AttachPreparedDictionary` in the
     FFI (this also closes the known "no prepared-dictionary reuse" issue).

   Keep `set_custom_dictionary` as the legacy path. Expected impact vs current
   Rust:
   * q3: 7-65% smaller (JS deltas 22-37%, bootstrap 65%, wiki about 7%);
   * q5/q6: 1-45% smaller;
   * q7-q9: mixed, up to about 8% larger (jquery q9 6496 -> 7029, vue
     +2.7%) and slightly smaller on bootstrap/lodash. Speed: one
   extra chain walk per position (C's cost), but no per-stream
   dictionary hashing if the prepared dictionary is cached.

### D4. q10/q11: Rust emits dictionary-spanning copies that both decoders reject (CONFIRMED correctness bug)

1. **Affects:** q10/q11 whenever the dictionary ends with a string that also
   starts the input. A short-range match then begins in the dictionary and
   continues past its end into the input. The corpora don't trigger it
   (verify passes), but a 4.8 KB repro does: the dictionary ends in
   `"The quick brown fox ... dog. "` and the input repeats that phrase.
   * `brotli_compare --verify --corpus docs/encoder-parity/repro/dict-span/input.txt
     --dictionary docs/encoder-parity/repro/dict-span/dict.bin`: q11 gives
     `OK/OK/ERR/ERR [FAIL]` (`ERROR_COMPOUND_DICTIONARY` from both C and
     Rust decoders).
   * A scratch fuzzer (random dictionary/input pairs; 60 seeds x q2-q11 x
     static/flushed = 1200 configurations) gives **84 undecodable Rust
     streams: 18 at q10, 66 at q11, none at q2-q9**.
2. **First difference:** command #1. Rust emits `copy 1800, distance 45` at
   pos 0. C emits `copy 45 (dictionary)` + `copy 1755, distance 45 (window)`.
3. **Root cause:**
   * The decoders forbid compound copies past the end of the dictionary
     (`c/dec/decode.c:1596`; the Rust decompressor behaves the same way).
     C's encoder never produces them because compound matches are limited
     to `source_size - offset` (`c/enc/hash.h:585-586,618-619`).
   * Rust clamps matches at the dictionary boundary with `fix_unbroken_len`
     (`src/enc/backward_references/mod.rs:42-54`), applied in every hasher
     and in Zopfli `UpdateNodes` (`hq.rs:738-747`) and `StoreAndFindMatchesH10`
     (`hash_to_binary_tree.rs:485-494`). The exception is the short-distance
     linear scan in `FindAllMatchesH10` (`src/enc/backward_references/hq.rs:347-349`),
     which uses a 16-byte window at q10 and 64 bytes at q11.
4. **Evidence:** wrapping that `len` in `fix_unbroken_len(len, prev_ix,
   cur_ix_masked, ring_buffer_break)` in the scratch copy:
   * The fuzzer reports 0/1200 failures.
   * The span repro at q11 becomes **bit-exact with C** (3033 B).
   * All 8 corpus cases at q11 are byte-identical to the original Rust
     output.
5. **Fix:** apply that one-line clamp upstream (it is Chrome-relevant: such
   streams fail to decode). Compression impact is a few bytes, only in
   affected streams. Also consider debug-asserting in the bit writer that
   no copy crosses `custom_dictionary_size`.

### D5. Why C's prepare is expensive even at q0/q1 (CONFIRMED)

* **Symptom:** C q1 encode of wiki-Brotli.html drops from 169.9 to 63.3
  MiB/s with the 64 KB dictionary, while the output is byte-identical.
  (`bench_c --op encode --quality 1`, 300 iterations, CPU 30.) Rust drops
  from 108.9 to 91.3 MiB/s; that drop is D1 and goes away with the fix.
* **Cause:** the harness times prepare + attach on every encode. For RAW,
  `BrotliEncoderPrepareDictionary` ignores `quality` (`c/enc/encode.c:1805`)
  and always runs `CreatePreparedDictionary`
  (`c/enc/compound_dictionary.c:13-169`):
  * a 1.3 MB temporary allocation (`num`, `bucket_heads`, `next_bucket`,
    with 2^17 buckets minimum);
  * a 256 KB `memset`;
  * hashing every dictionary position with scattered writes;
  * two passes over all 2^17+ buckets (slot limits, then chain flattening);
  * a second allocation of about 0.5-0.8 MB.

  None of this is used at q0-q2.
* **Measurements (`prep_bench`, prepare + destroy, CPU 34):**
  * 5 KB dictionary: 0.41 ms (fixed bucket overhead)
  * 64 KB: 1.32 ms
  * 90 KB: 1.95 ms
  * 233 KB: 2.61 ms

  For comparison, the whole no-dictionary q1 encode of the 238 KB page
  takes 1.34 ms.
* **Profile:** `perf record` at the same iteration count shows:
  * `BrotliCompressFragmentTwoPassImpl17` at the same absolute sample count
    in both runs (6580 vs 6528);
  * `CreatePreparedDictionary` at about 45% of the dictionary run;
  * kernel samples (page faults on the fresh allocations) up by about 6.2K,
    about 15% of the run.
* **Implications:**
  * Real servers should prepare once per dictionary and reuse the result;
    the API exists for that.
  * At q0/q1 a server can skip the dictionary altogether (identical
    output).
  * For benchmarking, either cache the prepared dictionary on the C side or
    report prepare cost separately; otherwise q0-q4 dictionary rows mostly
    measure `CreatePreparedDictionary`.
  * Rust's legacy path has the opposite profile: it is free at q0/q1 (once
    D1 is fixed), but at q >= 2 it pays per-stream for copying the
    dictionary into the ring buffer and inserting every position, with no
    reuse (`set_custom_dictionary_with_optional_precomputed_hasher` takes a
    precomputed hasher internally, but the FFI doesn't expose it).
