# brotli-bench

C/C++ benchmark and verification harness to test, profile, verify parity, and optimize both the Rust brotli encoder (`rust-brotli`) and decompressor (`rust-brotli-decompressor`) via their C FFI bindings against Google's reference C implementation (`brotli-c`).

See [AGENTS.md](AGENTS.md) for working rules, known issues, and the optimization log; [docs/development.md](docs/development.md) for the branch / PR workflow; [docs/baseline.md](docs/baseline.md) for the pre-optimization baseline; and [docs/encoder-parity.md](docs/encoder-parity.md) for why Rust encoder output differs from C.

## Submodules

- `vendor/brotli-c`: Google's C reference implementation (`https://github.com/google/brotli.git`)
- `vendor/rust-brotli-decompressor`: Rust decompressor (`https://github.com/pmeenan/rust-brotli-decompressor.git`, `upstream` -> `dropbox/rust-brotli-decompressor`)
- `vendor/rust-brotli`: Rust encoder & C FFI crate (`https://github.com/pmeenan/rust-brotli.git`, `upstream` -> `dropbox/rust-brotli`)

The submodules are used unmodified by the build: `rust/` is a small wrapper crate that builds the `vendor/rust-brotli/c` FFI as `libbrotli_bench_ffi.a` and patches `brotli` / `brotli-decompressor` to the submodule paths (with a committed `Cargo.lock`).

## Build

```bash
git submodule update --init
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Optional CMake cache variables: `CARGO_EXECUTABLE`, `BROTLI_BENCH_RUST_LTO` (default `fat`), `BROTLI_BENCH_RUST_PANIC` (default `abort`), `BROTLI_BENCH_RUSTFLAGS` (default `-C target-cpu=native`), `BROTLI_BENCH_C_ARCH_FLAGS` (default `-march=native`).

This builds three executables in `build/`:
1. `bench_c`: Pure C reference binary linked against `libbrotli_c.a`.
2. `bench_rust`: Pure Rust binary compiled from the exact same C++ source and `<brotli/*.h>` headers as `bench_c`, linked against `libbrotli_bench_ffi.a`. Supports `--rust-api <ffi|direct>` (default `ffi`) to benchmark either the C FFI (`BrotliDecoderDecompressStream`) or a thin wrapper around the direct Rust streaming decoder (`BrotliState::new` + `BrotliDecompressStream`).
3. `brotli_compare`: Side-by-side differential verification and benchmark binary linking both the Rust wrapper (`--rust-api <ffi|direct>`) and the symbol-prefixed `libbrotli_c_prefixed.a` (`C_Brotli*`).

All three also link `libbrotli_c_prefixed.a`, which `bench_c` / `bench_rust` use only to pre-encode decode workloads so both decode byte-identical C-encoded streams.

## Corpora

- `vendor/brotli-c/tests/testdata`: `alice29.txt`, `lcet10.txt`, `asyoulik.txt`, `monkey`.
- `corpora/`: real-world JS/CSS (MIT, from cdnjs) and Wikipedia HTML (CC BY-SA 4.0). Every top-level non-`.md` file is auto-discovered.
- `corpora/dictionary/manifest.tsv`: compression dictionary transport cases using raw (prefix) dictionaries: JS/CSS delta updates (previous version as dictionary) and Wikipedia HTML with a stand-alone generated dictionary.

Provenance, licenses, and compression ratios are in [corpora/SOURCES.md](corpora/SOURCES.md). Re-fetch/regenerate with `scripts/fetch_corpora.sh`.

## Usage

### 1. Parity & Cross-Decoder Verification
```bash
./build/brotli_compare --verify                         # everything, q0-11, static + flushed
./build/brotli_compare --verify --set dictionary --quality 5-6
./build/brotli_compare --verify --lgwin 18 --corpus wiki-Brotli.html --dictionary big.dict  # dictionary larger than the window
```

### 2. Side-by-Side Benchmark Suite
```bash
./build/brotli_compare --bench                          # default case subset, q0-11
./build/brotli_compare --bench --decode-only --rust-api direct   # decode-only, direct Rust API
# Filter by case (comma-separated names, substrings, or file paths), mode, or quality:
./build/brotli_compare --bench --corpus wiki-Brotli.html,dict:jquery --mode flushed --quality 5
```

Throughput is the median MiB/s of uncompressed data with `+/-` half the interquartile range. Each cell runs at least `--min-iters` (3) timed iterations and `--min-time-ms` (300) per implementation after a warmup, interleaving C and Rust.

### 3. Single-Target Profiling (`perf` / `samply`)
```bash
./build/bench_c    --op decode --mode flushed --quality 6 --input corpora/wiki-Brotli.html --iterations 200
./build/bench_rust --op decode --mode flushed --quality 6 --input corpora/wiki-Brotli.html --iterations 200
# With a raw dictionary (delta update):
./build/bench_rust --op decode --quality 5 --input corpora/jquery-3.7.1.min.js \
  --dictionary corpora/dictionary/jquery-3.6.4.min.js --iterations 500
```
