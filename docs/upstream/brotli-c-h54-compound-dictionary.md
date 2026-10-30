# Draft issue for google/brotli: raw dictionary ignored at quality 4 for inputs >= 1 MiB

Status: **draft, not filed**. File at https://github.com/google/brotli/issues.

---

**Title:** Encoder silently ignores attached raw (compound) dictionaries at
quality 4 when size hint >= 1 MiB (H54), and at quality 3 with large window
(H35)

**Summary**

`BrotliCreateBackwardReferences` only uses the compound dictionary for the
hasher types listed in its `CreateBackwardReferencesDH*` switch
(`c/enc/backward_references.c`, the `if
(params->dictionary.compound.num_chunks != 0)` block): 3, 4, 5, 6, 40, 41, 42,
55, 65 (and 58/59/68 with SIMD). It does not list:

- **H54**: `ChooseHasher` picks it at quality 4 when `size_hint >= 1 << 20`
  and `lgwin <= 24` (`c/enc/quality.h`).
- **H35**: quality 3 with `lgwin > 24`.

For those hashers the switch hits `default: BROTLI_DCHECK(BROTLI_FALSE);`
and falls through to the no-dictionary `CreateBackwardReferencesNH*` path:
- **Release builds:** the attached dictionary is silently unused, but
  static-dictionary distances are still shifted by the dictionary size.
- **`BROTLI_DEBUG` / `BROTLI_ENABLE_LOG` builds:** the `DCHECK` asserts.

Quality 2 (H2) is also not listed, but that appears to be intentional.

**Repro** (brotli 1.2.0+, `d89c4b1`; `dict.bin` = any raw dictionary related
to the input, e.g. a 64 KB dictionary trained on similar HTML pages):

```
$ brotli -q 4 -c big.html | wc -c                   # big.html = 1,200,000 bytes
180671
$ brotli -q 4 -D dict.bin -c big.html | wc -c
180671                                             # dictionary unused
$ brotli -q 3 -D dict.bin -c big.html | wc -c       # q3: 189293 without dictionary
185895
$ brotli -q 5 -D dict.bin -c big.html | wc -c       # q5: 159468 without dictionary
157520
$ head -c 900000 big.html > medium.html             # below 1 MiB, q4 uses H4
$ brotli -q 4 -c medium.html | wc -c
139012
$ brotli -q 4 -D dict.bin -c medium.html | wc -c
136417                                             # dictionary used
```

The CLI sets the size hint from the file size. The same happens with the
streaming API when the whole input is provided before the first
`BrotliEncoderCompressStream` call, or with an explicit
`BROTLI_PARAM_SIZE_HINT`.

**Expected:** an attached raw dictionary is used at q4 regardless of input
size, as it is at q3 and q5-q11.

**Suggested fix:** instantiate `backward_references_inc.h` with
`ENABLE_COMPOUND_DICTIONARY` for H54 (and H35), and add `CASE_(54)` (and
`CASE_(35)`) to the `DH` switch.

A local patch doing this for H54 (along with H2) produced the following on
the 1.2 MB input above, with the 64 KB dictionary: 178109 bytes vs 180671,
and the stream decodes correctly with the dictionary. Encode speed went from
56.8 to 42.1 MiB/s, a similar relative cost to H4 with a dictionary.

Found while comparing the Rust port (dropbox/rust-brotli) against the C
encoder for Compression Dictionary Transport use cases.
