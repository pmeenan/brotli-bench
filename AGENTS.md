# AGENTS.md

Running memory and working instructions for agents (and humans) working on
`brotli-bench`. **Read this first, and update it and every affected doc
before you finish a task** (see
[Maintaining documentation](#maintaining-documentation)).

## Documentation index

| Doc | Read it when | Owner of |
| --- | --- | --- |
| `AGENTS.md` (this file) | Always, first | Rules, known issues, log |
| [`docs/development.md`](docs/development.md) | Before committing to a fork, bumping a submodule, or preparing a PR | Branch layout, per-commit gates, PR / rebase / tagging process, branch status |
| [`docs/decoder-perf-plan.md`](docs/decoder-perf-plan.md) | Before any decoder work (current priority) | Decoder perf goal, profiles, method, prioritized workstreams |
| [`docs/perf-host.md`](docs/perf-host.md) | Before any performance measurement on the `linux` box | Perf host specs, `hostlock` reservation protocol, measurement recipe |
| [`docs/baseline.md`](docs/baseline.md) (+ `docs/baseline/`) | Before and after any perf measurement | Pre-optimization numbers, methodology, noise / drift |
| [`docs/encoder-parity.md`](docs/encoder-parity.md) (+ `docs/encoder-parity/`) | Before touching encoder code; when output changes | Every Rust-vs-C output divergence, root causes, fix patches, repros |
| [`docs/compound-dictionary-plan.md`](docs/compound-dictionary-plan.md) | Before working on encoder dictionary support | Plan + prototype data for porting C's prepared/compound raw dictionaries (dictionaries larger than the window, q2 extension) |
| [`docs/upstream/`](docs/upstream/) | Before filing upstream issues | Drafts of upstream bug reports (and whether each is filed) |
| [`corpora/SOURCES.md`](corpora/SOURCES.md) | Before adding or changing corpora | Provenance, licenses, ratios, rejected candidates |
| [`README.md`](README.md) | Quick start | Build / usage summary |

## Goal

Improve the performance of the Rust brotli encoder (`rust-brotli`) and
decompressor (`rust-brotli-decompressor`) until they match or beat Google's C
reference implementation (`brotli-c`), **without changing output**:

- Decoder: must decode every valid stream byte-identically to C, and reject
  the same invalid streams.
- Encoder: must produce valid streams. Bit-exact parity with C is tracked and
  desirable but not required unless noted per-quality below.

**Current priority (2026-10-30): decoder performance**
([`docs/decoder-perf-plan.md`](docs/decoder-perf-plan.md)). Encoder work is
tabled apart from upstream review of dropbox/rust-brotli#278.

The target workload is Chrome-style streaming (`BrotliSourceStream`, 32KB
`IOBuffer`), both for static resources and for flushed/streamed responses,
with and without Compression Dictionary Transport raw dictionaries.

## Repository layout

| Path | What |
| --- | --- |
| `CMakeLists.txt` | Builds `brotli_c` (static C ref), runs `cargo` on `rust/` to build `libbrotli_bench_ffi.a`, generates `libbrotli_c_prefixed.a` (all C globals renamed `C_*` via `nm` + `objcopy`), and links the 3 binaries. |
| `rust/` | Wrapper crate (`Cargo.toml`, `lib.rs`, committed `Cargo.lock`): staticlib re-exporting `vendor/rust-brotli/c`, `[patch.crates-io]` to the submodules, release profile. All Rust build plumbing lives here, never in `vendor/`. |
| `src/streaming_harness.h` | Shared harness: `StandardBrotliApi` (unprefixed `<brotli/*.h>` symbols), `EncodeStream<Api>` / `DecodeStream<Api>` for `StreamMode::kStatic` / `kFlushed`, raw dictionary support, `TestCase` loading (standard corpora + dictionary manifest). |
| `src/c_reference_api.h` | `PrefixedCBrotliApi` (`C_*` symbols). Keep in sync with `StandardBrotliApi`. |
| `src/bench_util.h` | Strict CLI number parsing, `SampleStats` (median / IQR), MiB/s. |
| `src/bench_main.cc` | Single-implementation driver. Compiled twice: `bench_c` (C) and `bench_rust` (Rust FFI, `BROTLI_BENCH_STANDARD_API_IS_RUST`). Decode workloads are pre-encoded with the prefixed C reference in both. Use for `perf`/`samply`. |
| `src/compare_main.cc` | `brotli_compare`: links both (`RustApi` = unprefixed, `CApi` = `C_*`). `--verify` (bitstream parity + 4-way cross-decode + dictionary checks) and `--bench` (side-by-side table). |
| `corpora/` | Real-world inputs, auto-discovered (top-level regular files, not dot files, not `*.md`). Provenance/licenses/ratios in `corpora/SOURCES.md`. |
| `corpora/dictionary/` | Dictionary cases; `manifest.tsv` lines are `name<TAB>dictionary<TAB>input` (repo-relative). Cases load as `dict:<name>`. |
| `docs/development.md` | Branch / development plan for the forks, verification gates, PR workflow, branch status. |
| `docs/baseline.md`, `docs/baseline/` | Pre-optimization baseline (2026-10-29): raw bench/verify CSVs, environment, per-case tables. Compare new runs against `docs/baseline/bench-2026-10-29.csv`. |
| `docs/encoder-parity.md`, `docs/encoder-parity/` | Catalog of every Rust-vs-C encoder output divergence with root causes, a confirmed 8-patch fix series (`patches/`), and the dictionary-span repro. |
| `scripts/summarize_bench.py` | Markdown summary of a `--bench-csv` file; `--baseline OLD.csv` for before/after, `--details` for per-case tables. |
| `scripts/brstruct.py` | Diffable structural dump of a brotli stream (headers, block switches, context maps, commands) for finding the first C/Rust divergence. |
| `scripts/fetch_corpora.sh` | Re-downloads / regenerates corpora; checksums in `scripts/corpora.sha256` (outside `corpora/` so it isn't loaded as a corpus). |
| `vendor/brotli-c` | Submodule: google/brotli (upstream, read-only reference). |
| `vendor/rust-brotli` | Submodule: fork `pmeenan/rust-brotli` (`upstream` remote -> `dropbox/rust-brotli`). Encoder + C FFI crate in `c/`. |
| `vendor/rust-brotli-decompressor` | Submodule: fork `pmeenan/rust-brotli-decompressor` (`upstream` -> `dropbox/...`). |

Standard cases: `alice29.txt`, `lcet10.txt`, `asyoulik.txt`, `monkey` from
`vendor/brotli-c/tests/testdata`, plus everything at the top of `corpora/`
(MIT JS/CSS from cdnjs, CC BY-SA Wikipedia HTML). Dictionary cases: JS/CSS
delta updates (previous version as dictionary: jquery, react-dom, lodash,
vue, bootstrap) and 3 held-out Wikipedia pages with a 64KB stand-alone
dictionary generated by brotli's `research/dictionary_generator --dsh` from
20 other Wikipedia pages.

## Build

```bash
git submodule update --init
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

- C and Rust are both built with native CPU tuning (`-O3 -march=native -g`,
  `RUSTFLAGS=-C target-cpu=native`) and debug info for profiling. Benchmark
  numbers are therefore machine-specific; always record the machine/CPU with
  any numbers.
- Rust release profile (in `rust/Cargo.toml`): `opt-level=3`, `lto="fat"`,
  `codegen-units=1`, `panic="abort"` (Chromium builds Rust with
  `panic=abort`). Override with `-DBROTLI_BENCH_RUST_LTO=thin|off` /
  `-DBROTLI_BENCH_RUST_PANIC=unwind` (passed as `CARGO_PROFILE_RELEASE_*`).
  Use the same settings for every measurement you compare.
- `cargo` is found via `find_program` (`$CARGO_HOME/bin`, `~/.cargo/bin`,
  `~/.local/bin`); override with `-DCARGO_EXECUTABLE=...`. Cargo output goes
  to `build/cargo-target/`.
- The submodules build unmodified. If you bump a submodule and cargo needs a
  new lock entry, it updates `rust/Cargo.lock`; commit that.

## Run

```bash
# Correctness (always run before and after any optimization change)
./build/brotli_compare --verify                    # all cases, q0-11, static+flushed (~minutes)
./build/brotli_compare --verify --quality 5 --mode flushed
./build/brotli_compare --verify --set dictionary   # dictionary cases only

# Side-by-side benchmark (default subset: kDefaultBenchCases in compare_main.cc)
./build/brotli_compare --bench
./build/brotli_compare --bench --corpus wiki-Brotli.html,dict:jquery --mode flushed --quality 4-6

# Compare against the baseline
python3 scripts/summarize_bench.py /tmp/new.csv --baseline docs/baseline/bench-2026-10-29.csv

# Single-implementation profiling (prints median_time per stream, too)
perf record -g ./build/bench_rust --op decode --mode static --quality 6 \
  --input corpora/wiki-Brotli.html --iterations 500
./build/bench_c --op decode --quality 5 --input corpora/jquery-3.7.1.min.js \
  --dictionary corpora/dictionary/jquery-3.6.4.min.js --iterations 500
```

- `--corpus` takes comma-separated exact names, else substrings, else file
  paths; anything unmatched is an error. `--verify-csv` / `--bench-csv` write
  machine-readable results; `--dump-dir DIR` (verify) writes `*.c.br` /
  `*.rust.br` for every non-identical encode. `--quality` takes `N` or `A-B`.
  `--dictionary FILE` attaches a raw dictionary to every selected case.
  `--lgwin N` (10-24, default 22) sets the encoder window; `bench_c` /
  `bench_rust` also take `--lgwin`.
- `brotli_compare` exits non-zero if any verify check or any bench
  encode/decode fails. Bench checks every timed run (`ok`, deterministic
  encoded size, decoded size) and fully verifies decode output once per cell.
- Decode benchmarks always use the **C-encoded** artifact so both decoders
  process identical bytes (in `bench_c`/`bench_rust` too).
- Timing: output buffers are reserved before the timer starts. Encode timing
  includes instance create/destroy and dictionary prepare/attach (C:
  `BrotliEncoderPrepareDictionary` at the encode quality; Rust:
  `BrotliEncoderSetCustomDictionary`), i.e. no prepared-dictionary caching.
  Decode timing includes create/destroy and `BrotliDecoderAttachDictionary`.
- Bench cells: one warmup each, then C/Rust interleaved (alternating order)
  until both have `--min-iters` (3) samples and `--min-time-ms` (300).
  Reported as median MiB/s `+/-` half IQR.

## Working rules

1. **Verify first, then measure.** Run `brotli_compare --verify` before and
   after every change to Rust code. A perf win that breaks cross-decode or
   changes decoder output is a regression.
2. **Benchmark hygiene.** Performance numbers come from the physical `linux`
   host; always reserve it with `hostlock`, as described in
   [`docs/perf-host.md`](docs/perf-host.md).
   - Builds and `--verify` run under `hostlock shared`.
   - Timing runs and profiling run under `hostlock exclusive`, inside a
     `tune_benchmark_host.py run --keep-aslr` session: turbo off, frequency
     locked at base, SMT and NMI watchdog off.

   Beyond that, compare runs on the same machine, same build flags,
   quiet system. Prefer pinning (`taskset -c N`) and repeated runs; report
   the `+/-` spread, not just one number. Record before/after numbers in the
   log below. Encode at q8-q10 drifts 10%-2.5x between runs (page faults /
   THP on large per-stream allocations), so for encoder changes there, A/B
   against a back-to-back run of the unmodified build on the same core
   instead of the stored baseline CSV. Rust encode is also noisier than C
   (p90 spread 6.6% vs 2.6%).
3. **Where changes go.**
   - Optimization and parity work lives in `vendor/rust-brotli` and
     `vendor/rust-brotli-decompressor` (the forks). Follow
     [`docs/development.md`](docs/development.md):
     - Never commit to a fork's `master`.
     - Encoder work goes on the stack `fix-dict-span` -> `parity` -> `perf`;
       decoder work goes on `perf`.
     - Each commit passes the verification gates.
     - Push the fork branch *before* bumping the submodule pointer here.
   - Build plumbing (patches, profiles, features) goes in `rust/`, not the
     forks.
   - Do **not** modify `vendor/brotli-c`; it is the reference.
   - Keep changes upstreamable to `dropbox/*`: no harness-only hacks in the
     crates, keep `no_std`/feature flags working, preserve the public API.
4. **No `unsafe`.** The Rust code is headed for Chromium for its memory-safety
   benefits, so performance work must be entirely safe Rust (iterators, slice
   patterns, fixed-size array refs, bounds hoisting).
   - Do not add `unsafe` blocks or functions in either fork.
   - Don't enable or extend the crates' opt-in `unsafe` cargo features, and
     never benchmark with them.
   - The only tolerated `unsafe` is the existing C ABI boundary code in the
     FFI modules. Don't grow it; new FFI entry points copy the existing
     pointer-conversion pattern.
   - The decompressor's default build must keep compiling with
     `forbid(unsafe_code)`.
5. **Harness changes** must keep `bench_c` and `bench_rust` compiled from the
   same source, and keep `StandardBrotliApi` / `PrefixedCBrotliApi` in sync
   when adding API calls.
6. **Corpora.** Only MIT/BSD/Apache code or freely licensed content (record
   source + license in `corpora/SOURCES.md`, add to `fetch_corpora.sh` and
   `scripts/corpora.sha256`). Avoid synthetic or extremely compressible
   (>~10x) inputs; they over-weight back-reference copies.
7. **Code style.** C++17, Google C++ style. Quote symbol names in comments with
   backticks (`` `foo` ``), not vertical bars.

## Known issues / open items

- [ ] **Rust encoder correctness bug (dictionary, q10/q11):** Rust can emit a
  copy that starts in the raw dictionary and runs into the input. Both
  decoders reject the stream (`ERROR_COMPOUND_DICTIONARY`). The corpora don't
  trigger it; the repro in `docs/encoder-parity/repro/dict-span/` does. Fixed
  on branch `fix-dict-span` (`822b2dc`, patch 0008, finding D5); upstream PR
  dropbox/rust-brotli#278 (open).
- [ ] **Encoder output parity.** Rust differs from C at q2-q11; all causes are
  identified in `docs/encoder-parity.md`. Patches 0001-0008 give 204/204
  bit-exact output on the standard cases, but cost 5-15% Rust encode speed at
  some qualities. They are committed on the local `fix-dict-span` / `parity`
  branches of `vendor/rust-brotli` (not pushed; see `docs/development.md`).
  Baseline parity (static = flushed): q0-q1 17/17; q2-q4 1/17; q5 14/17;
  q6-q8 15/17; q9 2/17; q10-q11 1/17; dictionary cases 0/8 everywhere.
- [ ] **Rust per-stream encoder setup cost is huge** for small inputs:
  1-byte input takes 10-20 us at q2-q4, 70-1350 us at q5-q8, and about 7 ms
  at q10/q11, vs under 1 us for C (about 20 us at q9-q11). See
  `docs/baseline.md`. This is likely the biggest win for small or streamed
  responses.
- [ ] **Dictionary model differs (D3).** C uses a separate compound
  dictionary index; Rust prepends the dictionary to the window
  (`BrotliEncoderSetCustomDictionary`). Rust has no
  `BrotliEncoderPrepareDictionary` / `AttachPreparedDictionary`, so a
  prepared dictionary can't be reused. As a result:
  - q2: Rust is much smaller (C doesn't use the dictionary at all).
  - q3-q6: Rust is much worse.
  - q7-q9: Rust is slightly better.
  - Rust also disables the static dictionary when a custom dictionary is set
    (D4).

  Plan to port C's model: `docs/compound-dictionary-plan.md`.
- [ ] **Dictionary encode timings are not like-for-like.** The harness
  re-runs C's `BrotliEncoderPrepareDictionary` on every stream. That costs
  about 1.3 ms for a 64KB dictionary even at q0-q2, where it is unused.
  Consider caching the prepared dictionary on the C side, or reporting
  prepare time separately.
- [ ] **Decoder error strings differ:** C returns `_ERROR_...` (leading
  underscore), the Rust FFI returns `ERROR_...`. The error codes match.
- [ ] **Harness only tests lgwin 22.** Rust q5-q9 at lgwin <= 16 fall back to
  H6 (C uses H40-H42), so their output differs from C and is identical
  across those qualities. Consider adding lgwin variants to `--verify`.
- [ ] `vendor/rust-brotli-decompressor` emits about 19 `missing_abi` warnings
  (`extern fn` without `"C"` in `src/ffi/mod.rs`). Harmless. The fix is a
  mechanical `extern fn` -> `extern "C" fn`; it existed as an uncommitted
  local edit, which was removed when build plumbing moved to `rust/`.
  Commit it to the fork if wanted.
- [ ] **Rust raw dictionary scrolls out of the window (D6).** The dictionary
  is truncated to `2^lgwin - 16`, and it becomes unreachable once
  `dict_size + pos` exceeds the window. C keeps it addressable.
  - Confirmed with an 8 MB dictionary: Rust at lgwin 18 compresses
    `wiki-Brotli.html` to 40601 bytes at q3, vs 16730 for C (42264 with no
    dictionary).
  - The decoder (`brotli-decompressor` >= 6.0) is already fine.
  - Covered by `docs/compound-dictionary-plan.md`.
- [ ] Latent encoder issues not exercised by the corpora (details in
  `docs/encoder-parity.md`):
  - `FastLog2u16` truncates counts >= 65536;
  - hasher selection differs for inputs >= 1 MiB at q5-q9.
- [ ] Rust's `portable-float` feature lowers C parity (keep it off); the
  `float64` feature doesn't compile.

## Log

Append dated entries (newest last): what changed, why, and before/after
numbers with the exact command, machine, and commit SHAs.

- **2026-10-29** - Initial scaffolding: CMake build, harness, 3 binaries,
  synthetic corpora. `--verify --quality 5`: 14/14 cross-decode pass, 10/14
  bitstream-exact. No baseline perf numbers recorded yet.
- **2026-10-29** - Review fixes: build plumbing moved from submodule edits to
  `rust/` wrapper crate (pinned `Cargo.lock`, fat LTO + `panic=abort`); bench
  validates every run, warms up, interleaves, reports median/IQR with
  time-based iteration counts; strict CLI validation; trailing-input
  detection; allocation moved out of timed regions; flushed wire chunks are
  recorded as offsets. Replaced synthetic corpora with real cdnjs JS/CSS and
  Wikipedia HTML; added raw-dictionary support (encoder + decoder) and
  dictionary cases (JS/CSS delta updates, stand-alone HTML dictionary).
  Full `--verify` (17 standard + 8 dictionary cases, q0-11, static+flushed):
  600/600 pass, 200/600 bitstream-exact, 82s wall (single-threaded).
- **2026-10-29** - Baseline recorded in `docs/baseline.md` (CSV:
  `docs/baseline/bench-2026-10-29.csv`; machine GCE Skylake-SP 2.0GHz,
  pinned CPU 2; brotli-c d89c4b1, rust-brotli 6df909c, decompressor 2ed02ed).
  Rust vs C geomean:
  - decode: -11.6% static, -13.5% flushed;
  - encode (no dictionary): -35% static, -31% flushed.

  Rust encoder per-stream setup is up to about 7 ms (q10/q11) vs about 20 us
  for C. Encode at q8-q10 varies up to 2.5x between runs (page faults / THP
  on large per-stream allocations); decode varies under 1%. Encoder output
  divergences were root-caused in `docs/encoder-parity.md` by 4 parallel
  investigations. An 8-patch series makes all standard cases bit-exact
  (204/204) and fixes a q10/q11 dictionary correctness bug; it is not
  applied yet. Added `--verify-csv`, `--bench-csv`, `--dump-dir`,
  `bench_main` `median_time`, `scripts/summarize_bench.py` and
  `scripts/brstruct.py`.
- **2026-10-29** - Branch plan documented in `docs/development.md`;
  AGENTS.md gained a documentation index and doc-maintenance rules.
  - Created local branches in `vendor/rust-brotli`:
    - `fix-dict-span` (`822b2dc`, patch 0008);
    - `parity` (`27a1859`, patches 0001-0007);
    - `perf` (same as `parity`).
  - Created `perf` in `vendor/rust-brotli-decompressor` (`2ed02ed`).
  - `.gitmodules` now tracks `branch = perf` for both forks.
  - Gates (pinned CPU 6):
    - `fix-dict-span`: 600/600 decode, 200/600 exact, span repro passes.
    - `parity`: 600/600 decode, 440/600 exact (408/408 standard), span
      repro passes.
  - Not pushed; superproject pointers are unchanged.
- **2026-10-29** - Pushed fork branches over SSH:
  - `pmeenan/rust-brotli`: `fix-dict-span`, `parity`, `perf`;
  - `pmeenan/rust-brotli-decompressor`: `perf`.

  The scaffolding commit was missing the submodule gitlinks (it had only
  `.gitmodules`). Added them, pinned at brotli-c `d89c4b1`, rust-brotli
  `perf` `27a1859`, and decompressor `perf` `2ed02ed`.
- **2026-10-29** - Switched the local fork `origin` remotes (and the
  superproject's local `submodule.*.url`) to SSH
  (`git@github.com:pmeenan/<repo>.git`); HTTPS prompted for credentials.
  `.gitmodules` keeps HTTPS URLs so anonymous clones work. After a fresh
  clone, either `git submodule update --init`, then
  `git -C vendor/<repo> remote set-url origin git@github.com:pmeenan/<repo>.git`,
  or edit the superproject's `submodule.<path>.url` before
  `git submodule update --init`.
- **2026-10-29** - Opened dropbox/rust-brotli#278 from `fix-dict-span`
  (`822b2dc`): "Fix matches that run past the end of a custom dictionary".
- **2026-10-30** - Planned porting C's compound/prepared raw dictionary to
  the Rust encoder (`docs/compound-dictionary-plan.md`). Added `--lgwin` to
  `brotli_compare`, `bench_c` and `bench_rust` (the CSVs gain an `lgwin`
  column).
  - The rust-brotli 9.0.0 README note about "compound dictionaries" is about
    the decoder only; the encoder still uses the prefix model.
  - Measured on CPU 6 with an 8 MB dictionary: C's output doesn't depend on
    lgwin; Rust's degrades to near no-dictionary at lgwin 18.
  - Prototyped C with compound-dictionary variants for H2/H54 plus a chain
    cap (scratch build). q2 + compound dictionary is 30-60% smaller than
    current Rust q2, and up to about 3x faster with prepare-once.
    - jquery delta: 7011 bytes at 73.6 MiB/s, vs Rust 11991 at 59.4.
    - 8 MB wiki dictionary: 16943 at 72.9 MiB/s, vs Rust 24251 at 28.3.
  - C prepare costs about 110 ms and a 9.8 MB index for an 8 MB dictionary,
    so prepare-once is required.
- **2026-10-30** - Recorded the compound-dictionary plan decisions:
  - FFI is lean/borrowed like C;
  - `SetCustomDictionary` is re-routed to the compound model;
  - large-dictionary data comes from a fetch script only.

  Confirmed with the C CLI that C ignores raw dictionaries at q4 for inputs
  >= 1 MiB (H54): a 1.2 MB input gives 180671 bytes with and without `-D`;
  900 KB uses it. Drafted the google/brotli issue in
  `docs/upstream/brotli-c-h54-compound-dictionary.md` (not filed). q2 is
  treated as a deliberate C choice.
- **2026-10-30** - Re-prioritized: decoder performance first; encoder work
  tabled. Wrote `docs/decoder-perf-plan.md`. First decode profiles (`perf`
  cpu-clock, CPU 6; the VM has no hardware PMU and no valgrind):
  - C and Rust spend time in the same functions, in the same proportions:
    `ProcessCommands` is 57-92%, and in flushed mode about 20% is
    prefix-code setup.
  - The gap is spread across the hot loop.
  - Rust-only extras: memset 1-2%, `SafeReadSymbol` 1.7% (flushed).
  - Profile-run speeds (C vs Rust MiB/s): wiki q1 static 346 vs 297; wiki q5
    static 383 vs 342; wiki q5 flushed 269 vs 236; jquery q11 176 vs 158.
- **2026-10-30** - Policy: no `unsafe` in performance work (Chromium
  security). Updated working rule 4, `docs/decoder-perf-plan.md` (gates:
  default build with `forbid(unsafe_code)`, no added `unsafe` in diffs) and
  `docs/development.md`. Confirmed that the harness build doesn't enable
  brotli-decompressor's `unsafe` feature (features: `std`, `alloc-stdlib`,
  `ffi-api`), so the baseline measures the safe decoder.
- **2026-10-30** - Documented the physical perf host `linux` and its
  `hostlock` reservation protocol in `docs/perf-host.md`.
  - Host: i9-11900KF 8C/16T, PMU counters available, `powersave` + turbo.
  - `hostlock` is `~/.local/bin/hostlock`, with the lock dir `~/.hostlock`.
  - `cmake` is not installed there yet.
  - At the time of checking, another project (`chunky: cache-bench`) held
    the exclusive lock.
- **2026-10-30** - Added noise control to `docs/perf-host.md`: the
  optimize-campaign `tune_benchmark_host.py` tuner (turbo off,
  `performance` governor, min = max = 3.5 GHz base, NMI watchdog off, SMT
  off, ASLR kept on for timing runs), the canonical
  `hostlock exclusive` + tuner + `taskset` command, and crash recovery
  (`tune_benchmark_host.py disable`).

## Maintaining documentation

Docs are the project's memory; stale docs are bugs. Before finishing any
task, update every doc your change affects, in the same change:

- **`AGENTS.md`:**
  - **Known issues:** add what you found; check off or remove what you
    fixed (link the commit).
  - **Log:** one dated entry per task or optimization attempt, *including
    failed or reverted ones* (what was tried, why it didn't help), with
    commands, machine, commit SHAs, and before/after numbers.
  - **Instructions:** keep build / run / CLI instructions accurate when
    behavior changes.
- **`docs/development.md`:** update the *Current status* table whenever a
  branch is created, rebased, pushed, or merged upstream, or a PR is opened.
  Update the plan if the branch layout or gates change, including the
  required exact-match counts.
- **`docs/encoder-parity.md` (+ `docs/encoder-parity/`):** when an encoder
  change alters output, or a divergence is fixed or newly found:
  - update the catalog and the parity tables;
  - mark landed fixes with their branch / commit;
  - keep `patches/` and the fork branches consistent (or note that the fork
    is now the source of truth).
- **`docs/baseline.md`:** never rewrite the 2026-10-29 baseline. If a new
  reference measurement is needed (new machine, harness change that affects
  timing), add a new dated CSV under `docs/baseline/` and a section
  explaining why. Use `scripts/summarize_bench.py --baseline` for
  comparisons.
- **`corpora/SOURCES.md`, `scripts/fetch_corpora.sh`,
  `scripts/corpora.sha256`:** keep in sync whenever corpora change.
- **`README.md`:** keep the quick start accurate for user-visible build /
  CLI changes.
- Keep docs concise; put long analyses in `docs/` and link them from here.
