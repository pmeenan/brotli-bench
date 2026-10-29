## Build-flag sensitivity of C and Rust encoder output (`build-flags`)

**Question:** is either encoder's output a function of compiler/target flags,
and can any flag combination increase C/Rust bit-exact parity?

**Short answer:** No flag changes the C reference output on this corpus
(except `-ffast-math`, at q10-q11), and no Rust codegen flag changes Rust
output. The only Rust knob that changes output is the `portable-float`
feature, and it *reduces* parity. So no flag combination explains or fixes
the q2-q11 divergences; they are source-level. While working through the
float side I found (and confirmed with a scratch patch) a source-level q10/q11
cause: Rust `FastLog2f64` computes `log2` in `f32`, but C computes it in
`double` (section 4).

Setup: 17 standard cases (4 `testdata` + 13 `corpora/*`), q0-q11, static mode
(`BrotliEncoderCompressStream` streaming API, lgwin 22, mode GENERIC, PROCESS
the whole input then FINISH, 32KB output buffer, no size hint). The driver
`encall.c` is linked against each variant library. Its output for `build/`
libs is byte-identical to the `brotli_compare --dump-dir` streams in
the `brotli_compare --dump-dir` streams (checked on alice29 q3/q5/q9/q11). Machine: GCE Xeon
(skylake-avx512, FMA/AVX2/AVX-512), gcc 15.2, clang 19, Chromium rustc
1.101-nightly (LLVM 24), glibc 2.43. Sources: brotli-c `d89c4b1`, rust-brotli
`6df909c`. Everything was pinned to `taskset -c 24-47`.

The experiment drivers (a static-mode encoder linked against each library
variant, plus build/compare scripts) were scratch tools and are not
committed. The `FastLog2f64` fix is part of the combined patch series in
[patches/](patches/).

### 1. C reference: output is flag-invariant (confirmed)

Exact matches against the current `build/libbrotli_c.a` (`-O3 -march=native
-g -std=gnu11`), per quality q0..q11, out of 17:

| C variant | q0-q9 | q10 | q11 | total |
| --- | --- | --- | --- | --- |
| scratch rebuild, same flags | 17 each | 17 | 17 | 204/204 |
| `-O2 -march=native` | 17 | 17 | 17 | 204/204 |
| `-O0` (generic x86-64) | 17 | 17 | 17 | 204/204 |
| `-O3 -march=x86-64` / `x86-64-v2` / `x86-64-v3` | 17 | 17 | 17 | 204/204 |
| `-O3 -march=native -ffp-contract=off` | 17 | 17 | 17 | 204/204 |
| `-O3 -march=native -std=c11` (ISO, no contraction) | 17 | 17 | 17 | 204/204 |
| clang-19 `-O3 -march=native` (with / without `-ffp-contract=off`) | 17 | 17 | 17 | 204/204 |
| `-U__SSE2__` (scalar `GetMatchingTagMask`) | 17 | 17 | 17 | 204/204 |
| runtime `BROTLI_PARAM_SIMD_HASHER=DISABLE` (H5 instead of H58) | 17 | 17 | 17 | 204/204 |
| runtime `BROTLI_PARAM_SIMD_HASHER=ENABLE` (H58 also at q7) | 17 | 17 | 17 | 204/204 |
| `nocontract` + `SIMD_HASHER=DISABLE` | 17 | 17 | 17 | 204/204 |
| glibc libm without FMA/AVX ifuncs (`GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA,-AVX512*,...`) | 17 | 17 | 17 | 204/204 |
| **`-O3 -march=native -ffast-math`** | 17 | **1** | **1** | 172/204 |

Details:

- **FMA contraction is real but doesn't change output.** gcc in `gnu11` mode
  defaults to `-ffp-contract=fast`. The `-march=native` (and `x86-64-v3`)
  build has 92 `vfmadd`/`vfmsub` instructions; `-O2` has 43, clang 122,
  `-ffp-contract=off` / `-std=c11` / `x86-64(-v2)` have 0. They sit in
  `BrotliPopulationCost{Literal,Command,Distance}`, `BrotliBitsEntropy`,
  `BrotliCompareAndPushToQueue*` (clustering), `BrotliSplitBlock`,
  `ShouldMergeBlock`, `BrotliEstimateBitCostsForLiterals` and
  `WriteMetaBlockInternal`. Even so, all 204 streams are identical with and
  without FMA. No cost comparison on this corpus sits close enough to a tie
  for the last-ulp difference to flip it.
- **The SIMD hashers are active on gcc/Linux at q5-q6 and give identical
  output.** `BROTLI_TZCNT64` is `__builtin_ctzll` for any GNU compiler
  (`c/common/platform.h:541-543`), so `BROTLI_MAX_SIMD_QUALITY` is always
  defined on x86-64 Linux, with `BROTLI_MAX_RECOMMENDED_SIMD_QUALITY = 6`
  (`platform.h:663-669`; the `7` branch is MSVC-only via `_M_X64`). This does
  not depend on `-march`. `ChooseHasher` (`c/enc/quality.h:160-205`) then
  picks **H58** (`hash_longest_match_simd_inc.h`) instead of H5 at q5 and q6.
  I confirmed this with gdb breakpoints on `CreateBackwardReferencesNH*`:
  default q5/q6 -> NH58, q7/q8 -> NH5; `SIMD_HASHER=2` -> NH5 at q5-q8;
  `SIMD_HASHER=1` -> NH58 at q5-q7. H59/H68 aren't reached (`hasher_opt` is
  off; H68 needs `size_hint >= 1MB`).
  - H58 uses the same 14-bit bucket key as H5 (`hash >> 8` with
    `hash_shift = 32 - 14 - 8`). It filters candidates by an 8-bit tag taken
    from the next hash bits, and walks them newest to oldest like H5. A
    candidate with a different tag has different first 4 bytes, so it can't
    be a valid H5 match either. In practice the two are output-equivalent:
    q5-q7 are bit-identical with H5 vs H58 on all 17 cases.
  - `matching_tag_mask.h`: SSE2 is baseline on x86-64, so `-march` never
    switches the mask path. Forcing the portable bit-trick path
    (`-U__SSE2__`) is also bit-identical.
  - **Implication for the port:** Rust only has H5-style hashers, and that is
    fine. H58 doesn't explain any of the q5/q6 divergence, and porting it
    would be a pure performance change.
- **libm dispatch:** glibc's `double log2` has FMA and non-FMA ifunc variants
  that differ by 1 ulp on 370 of the 33.5M integers in [256, 2^25). None of
  those integers is below 2^16 and only 13 are below 2^20. `log2f` and `powf`
  are identical across variants. This does not change any of the 204 C
  streams. In principle the C output can depend on the host CPU's libm
  variant for histograms with very large counts; Rust (using `log2f`) is not
  exposed to this.
- **`-ffast-math`** changes q10-q11 on 16/17 cases (sizes q10 752116 ->
  751663, q11 736603 -> 736659) and nothing at q≤9. It doesn't move C toward
  Rust (1/17 at q10/q11 against Rust either way).

### 2. Rust: output is codegen-invariant; only `portable-float` changes it (confirmed)

Exact matches against the current `build/cargo-target` lib (`-C
target-cpu=native`, fat LTO, `panic=abort`):

| Rust variant | q0-q3 | q4 | q5 | q6 | q7 | q8 | q9 | q10 | q11 | total |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| rebuild, same flags | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 204/204 |
| default target-cpu (x86-64) | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 204/204 |
| `lto = off` | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 204/204 |
| glibc without FMA/AVX ifuncs | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 17 | 204/204 |
| `portable-float` (native or default cpu, identical) | 17 | 15 | 12 | 15 | 13 | 13 | 17 | 0 | 1 | 154/204 |
| `float64` feature | does not compile (32 type errors in `block_splitter.rs`, `prior_eval.rs`, `vectorization.rs`) | | | | | | | | | |

- Rust/LLVM never contracts `a*b+c` without explicit `mul_add`, so
  `target-cpu` can't change float results, and the encoder has no
  runtime-dispatched SIMD that affects decisions.
- **`portable-float`** (`Cargo.toml:72`, `src/enc/util.rs:17-58,87+`) swaps
  `log2`/`powf` for the table-based `FastLog2u64`/`FastPow2`
  approximations. Against C it lowers parity from 100/204 to **85/204**
  (q5 14->10, q6 15->13, q7 15->11, q8 15->11, q10 1->0). It also costs about
  +0.07% at q10 and +0.14% at q11 in total size, with q4-q9 within ±0.01%.
  For C parity it must stay **off**: the default libm path is the closer
  one.
- The `float64` feature is bit-rotted: it doesn't compile. Even if fixed, its
  std `FastLog2` would still be `(v as f32).log2()`.

### 3. C vs Rust parity under any flag combination (confirmed)

C/Rust parity is the same for every C variant above (including
`-ffp-contract=off`, `x86-64`, `-O0`, clang, H5-only): **q0-q1 17, q2-q4 1,
q5 14, q6-q8 15, q9 2, q10-q11 1 (100/204)**. Even `-ffast-math` C leaves the
per-quality counts against Rust unchanged. In particular, **C with
`-ffp-contract=off` does not match Rust on alice29/lcet10 at q5-q8 or on
anything new at q9-q11**. Flags are therefore ruled out as the cause of any
divergence; the other investigators' source-level findings stand.

### 4. Divergence found on the way: Rust `FastLog2f64` uses `f32` log2 (q10/q11)

1. **Affects:** q10 and q11 (Zopfli cost model initialisation from literal
   costs), all inputs. q≤9 are unaffected.
2. **First difference:** the literal cost array that seeds the Zopfli
   `ZopfliCostModelSetFromLiteralCosts` pass, so the command stream (copy
   choices) differs early in the first meta-block.
3. **Root cause:**
   - C `FastLog2` (`c/enc/fast_log.h:51-58`) returns `kBrotliLog2Table[v]`
     for `v < 256`. Note that the table entries are written as **float
     literals** (`0.0f`, `1.5849625007211563f`, `c/enc/fast_log.c:13-14`),
     so they are only f32-precise. For `v >= 256` it returns full `double`
     `log2((double)v)`. `BrotliEstimateBitCostsForLiterals`
     (`c/enc/literal_cost.c:111,164`) does `FastLog2(a) - FastLog2(b)` in
     double, then casts to float.
   - Rust `FastLog2f64` (`src/enc/util.rs:44-52`) returns `floatX` = `f32`
     and computes `(v as f32).log2()` for `v >= 256`. `literal_cost.rs:161-162,
     227` then widens to `f64` and subtracts, but the logs are already
     f32-rounded.
   - Rust's other `FastLog2` (`(v as f32).log2()`, `util.rs:26-31`) is *not*
     a source of divergence. C casts it to float immediately (`SetCost`,
     `backward_references_hq.c:127-142`), and switching Rust to
     `(v as f64).log2() as f32` gave byte-identical Rust output (204/204).
4. **Evidence:**
   - C scratch with `FastLog2` returning `(double)log2f((float)v)`: C output
     changes only at q10 (15/17) and q11 (16/17). Its parity *with Rust*
     rises from q10 1->12 and q11 1->6.
   - Rust scratch patch (now in [patches/](patches/)): `FastLog2f64` ->
     `f64` (`logs_8[v] as f64` below 256, `(v as f64).log2()` above), with
     the `SetCost` caller at `hq.rs:1062` cast back to `floatX` the way C does
     at `backward_references_hq.c:134`. Rust vs C at **q10 goes 1->14/17**
     (still differ: jquery.min, three, wiki-Espresso) and **q11 goes 1->4/17**
     (alice29, monkey, font-awesome, moment). q0-q9 Rust output is
     unchanged. Total parity 100->116/204.
   - Also fixing the prologue formula in `literal_cost.rs:169`
     (`0.7 - (2000-i)/2000*0.35` vs C `0.35 + (0.35/2000)*i`,
     `literal_cost.c:122`) changes nothing (identical output).
5. **Suggested fix:** make `FastLog2f64` return `f64` with the same mixed
   precision as C, and keep `portable-float` as a separate opt-in. Ratio
   impact is noise (q10 752152 -> 752125, q11 736542 -> 736655 total
   bytes). Perf impact: one double `log2` instead of `log2f` per symbol
   with a large count, only in the q10/q11 literal-cost pass. Expected to be
   negligible (not measured). The remaining q11 gap (13/17) and 3 q10 cases
   come from something else (probably the q11 two-pass cost model or
   f32-vs-double accumulation in Zopfli / block splitting); not investigated
   here.

**Confidence:** sections 1-3 and the item 4 numbers are confirmed by
experiment. The item 4 "remaining gap" explanation is a guess.
