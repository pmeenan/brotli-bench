# Development plan and branch workflow

How work on the Rust forks is organized, verified, and sent upstream. Keep
this file current as branches land or the plan changes (see
[AGENTS.md](../AGENTS.md#maintaining-documentation)).

## Priorities

Updated 2026-10-30.

1. **Decoder performance (active).** Work on `rust-brotli-decompressor:perf`
   per [decoder-perf-plan.md](decoder-perf-plan.md).
2. **Encoder (tabled).**
   - `fix-dict-span` stays in upstream review (dropbox/rust-brotli#278);
     respond to review, and rebase the stack if it lands.
   - `parity` stays as is.
   - Encoder perf work and the compound-dictionary port
     ([compound-dictionary-plan.md](compound-dictionary-plan.md)) wait until
     the decoder reaches its target. The plans may still be refined.

## Repositories

| Repo | Role | Remotes |
| --- | --- | --- |
| `brotli-bench` (this repo) | Harness, corpora, docs. Pins the forks as submodules. | - |
| `vendor/rust-brotli` | Encoder + C FFI. All encoder parity and performance work happens here. | `origin` = `pmeenan/rust-brotli`, `upstream` = `dropbox/rust-brotli` |
| `vendor/rust-brotli-decompressor` | Decoder. Decoder performance work happens here. | `origin` = `pmeenan/rust-brotli-decompressor`, `upstream` = `dropbox/rust-brotli-decompressor` |
| `vendor/brotli-c` | C reference. Read-only; never modified. | `origin` = `google/brotli` |

## Branch layout

Parity fixes and performance work touch the same encoder code:
`backward_references/mod.rs`, `block_splitter.rs`, `encode.rs`,
`backward_references/hq.rs`. So each fork uses **one linear stack** ordered
by dependency, not independent parallel branches.

```
rust-brotli:
  master            == upstream/master; never committed to
   └─ fix-dict-span    D5 correctness fix (dictionary-spanning copies at q10/q11)
       └─ parity       E1-E9, D1: encoder output bit-identical to C on the standard cases
           └─ perf     encoder performance work; submodule tracks this branch

rust-brotli-decompressor:
  master            == upstream/master; never committed to
   └─ perf          decoder performance work; submodule tracks this branch
```

- **`master`** in each fork mirrors `upstream/master`. To sync:
  `git fetch upstream && git checkout master && git merge --ff-only upstream/master && git push origin master`.
- **`fix-dict-span`** is a single commit (patch 0008 in
  [encoder-parity/patches/](encoder-parity/patches/)). It is a real
  correctness bug that is small and independent, so it goes upstream first.
- **`parity`** holds patches 0001-0007. With them, Rust encoder output is
  bit-identical to C on all 17 standard cases at q0-q11, static and flushed,
  and on the dictionary cases at q0/q1. See
  [encoder-parity.md](encoder-parity.md) for each finding.
- **`perf`** is where optimization commits go, on top of parity. Once outputs
  match C, any change in output during perf work is a bug. That makes
  `brotli_compare --verify` exact-match counts a strict regression gate.
  The first perf targets are the 5-15% encode slowdowns the parity patches
  introduce, at q2-q4, q6, q8-q11.
- The decoder needs no parity work: decode output already matches C
  everywhere; only the error strings differ. Its `perf` branch starts at
  `master` and stays independent of the encoder stack.
- Future independent topics get their own branch in the stack, after
  `parity` and before or after `perf` depending on what they touch.
  - **Planned:** `compound-dict` (C's prepared/compound raw dictionary
    model, see [compound-dictionary-plan.md](compound-dictionary-plan.md)).
    It goes between `parity` and `perf`.

The superproject records exact commit SHAs, not branch names. `.gitmodules`
sets `branch = perf` for both forks, so `git submodule update --remote`
follows the top of each stack once it is pushed.

## Commit rules

- **Small, self-contained commits.** Each one must build, pass
  `cargo test --release --lib` in the fork, and pass the harness gates below.
  This is what makes it possible to slice the stack into upstream PRs later.
- **Commit messages:** a `enc:` / `dec:` prefix, what changed, why, and the
  measured effect (parity counts or before/after MiB/s with machine and
  command).
- Keep `no_std`, feature flags, and the public API working. No harness-only
  hacks in the crates (see AGENTS.md > Working rules).
- **No `unsafe`** (AGENTS.md rule 4): no added `unsafe` in any commit, and no
  use of the crates' opt-in `unsafe` features.
- **Comments** (AGENTS.md rule 7): explain non-obvious target-specific or
  codegen-driven decisions in code comments, and check that any existing
  comments near modified code still accurately describe what the function does.
  Quote symbol and variable names with backticks (`` `foo` ``), never vertical
  bars.

## Verification gates (per commit)

Run from the superproject with the submodule checked out at the commit:

```bash
cmake --build build -j
./build/brotli_compare --verify --verify-csv /tmp/verify.csv   # must exit 0 (600/600 decode parity)
./build/brotli_compare --verify --quality 10-11 \
  --corpus docs/encoder-parity/repro/dict-span/input.txt \
  --dictionary docs/encoder-parity/repro/dict-span/dict.bin    # must pass once fix-dict-span is in
```

Every branch also requires:
- the decompressor's tests pass on 32-bit ARM: `scripts/test_armv7.sh` and
  `scripts/test_armv7.sh --release` (`qemu-arm`, on the perf host under
  `hostlock shared`). The x86-64 and aarch64 test runs never build the 32-bit
  bit reader;
- `git diff upstream/master...<branch> | grep '^+.*unsafe'` is empty;
- the decompressor's default-feature `cargo build --release` compiles (it
  enforces `forbid(unsafe_code)`).
  - Run it with `--target-dir` outside the repo.
  - Delete the untracked `Cargo.lock` it creates in the submodule.

| Branch | Required `--verify` exact matches |
| --- | --- |
| `master` (baseline) | 200/600 |
| `fix-dict-span` | 200/600, plus the span repro passes |
| `parity` and everything above it | 440/600: 204/204 standard + dictionary q0/q1. Must not drop. |

Perf commits also need before/after numbers. Run a back-to-back A/B against
the parent commit on the same core (see [baseline.md](baseline.md) on
run-to-run drift) and summarize with `scripts/summarize_bench.py`.

## Updating the submodule pointer

1. Commit in the fork on the right branch, then push it:
   `git push origin <branch>`.
2. **Only after the push**, in the superproject: `git add vendor/rust-brotli`
   (or the decompressor) and commit, mentioning the fork branch and SHA.
   Never commit a superproject pointer to a fork commit that isn't pushed;
   fresh clones can't fetch it.
3. Add a log entry to AGENTS.md.

## Sending PRs upstream

- Open PRs from fork branches against `dropbox/*:master`, starting at the
  bottom of the stack (`fix-dict-span`, then `parity`, possibly split into
  hashers / f64 costs / dictionary quirks, then perf in reviewable chunks).
- GitHub PRs to the upstream repo can only target upstream branches. A
  stacked PR therefore also shows the commits of the PRs below it until those
  merge. Say "depends on #N" in the description.
- To split a branch into smaller PRs, create a PR branch at the right commit
  of the stack (`git branch pr-hashers <sha>`). No cherry-picking is needed,
  as long as the commits are ordered by dependency.
- After a lower PR merges:
  1. Sync `master`.
  2. Rebase the rest of the stack onto it with
     `git rebase --update-refs master perf`, which moves `parity` / `perf`
     labels together.
  3. Re-run the gates.
  4. Force-push the rebased branches.
- **Before rewriting any branch** that the superproject has pointed at, tag
  its current tip and push the tag
  (`git tag bench/<date>-<branch> && git push origin bench/<date>-<branch>`).
  Otherwise old superproject commits reference unreachable SHAs.

## Current status

Created and pushed to the forks on 2026-10-29. The fork `master` branches
equal `upstream/master`.

| Fork | Branch | Tip | Base | Content | Gates | Pushed |
| --- | --- | --- | --- | --- | --- | --- |
| rust-brotli | `fix-dict-span` | `822b2dc` | `master` (`6df909c`) | patch 0008 | verify 600/600 decode, 200/600 exact; span repro passes; cargo test 43/43 | yes |
| rust-brotli | `parity` | `27a1859` | `fix-dict-span` | patches 0001-0007 | verify 600/600 decode, 440/600 exact (408/408 standard, 32 dictionary q0/q1); span repro passes; cargo test 43/43 | yes |
| rust-brotli | `perf` | `27a1859` | `parity` | (empty) | same as `parity` | yes |
| rust-brotli-decompressor | `perf` | `73b34c7` | `master` (`2ed02ed`) | 13 commits (`0466359`, `1344c34`, `081c5d9`, `fd8b2e8`, `05ee043`, `6082a30`, `45d09e0`, `d536284`, `03ac41a`, `d889cc0`, `32e41d6`, `afe3536`, `73b34c7`): bit-reader/transform, metablock reuse & prefix-code setup, `ProcessCommandsInternal` & compound dict fast path, FFI `SubclassableAllocator` drop-glue & state copy elimination, 32-bit bit-reader & ARMv7 bounds-check fast paths, review hardening (32-bit reader under test, debug asserts, upstream guards), persistent `BrotliFastBitReader` & ARMv7 hot-loop optimizations, ARMv7 16-bit unaligned loads + FFI single-zero alloc + outer-loop register pressure fixes, 32-bit single-`ldr` `HuffmanCode` layout, 32-bit 24-bit `n_bits <= 8` refill + branchless distance decode + comment updates, `npostfix == 0` distance fast path for any `ndirect` + `memmove16` bounds GVN + `fill_context_trees` run caching, Huffman setup (`replicate_value_pow2`, XOR-fold, `SYMBOL_LISTS_OFFSET`) + `SafeProcessCommands` + command block-switch margin re-check, pre-resolved `distance_roots` + folded per-copy bounds check + power-of-two LUT masking + chunk-boundary stress test | verify 600/600 decode, 440/600 exact (x86, mac, and Android); span repro passes; default build (`forbid(unsafe_code)`) passes; cargo test x86-64 and armv7 (`scripts/test_armv7.sh`), debug and release | yes |

The superproject pins `vendor/rust-brotli` at `perf` (`27a1859`),
`vendor/rust-brotli-decompressor` at `perf` (`73b34c7`), and
`vendor/brotli-c` at `d89c4b1`. To measure the unpatched baseline encoder,
check out `master` (`6df909c`) in `vendor/rust-brotli` and rebuild.

Upstream PRs:

| PR | Branch | Title | State |
| --- | --- | --- | --- |
| [dropbox/rust-brotli#278](https://github.com/dropbox/rust-brotli/pull/278) | `fix-dict-span` | Fix matches that run past the end of a custom dictionary | open |
 Candidate standalone decoder cleanup:
`extern fn` -> `extern "C" fn` in `src/ffi/mod.rs` (silences ~19
`missing_abi` warnings).
