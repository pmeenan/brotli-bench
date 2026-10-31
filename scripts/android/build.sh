#!/bin/bash
# Usage: scripts/android/build.sh TAG [rust-target] [rustflags]
# Builds static Thumb-2 + NEON armv7 binaries of the current working tree and
# pushes them to the device as
# /data/local/tmp/brotli-bench/{brotli_compare,bench_rust}_TAG (and bench_c_rv).
#
# Rust staticlib:
# - default: built on `linux` (stable toolchain, under `hostlock shared`) from
#   an rsync'd copy in ~/src/brotli-bench-rv;
# - LOCAL=1: built here with the Chromium nightly toolchain and -Zbuild-std
#   (no hostlock wait). Only compare builds made with the same toolchain.
# C and linking are local (arm-linux-gnueabihf-gcc). SKIP_RUST=1 relinks only.
# Paths assume a dev VM with ~/src/brotli-bench and `ssh linux` + adb.
set -euo pipefail
TAG=$1
RT=${2:-thumbv7neon-unknown-linux-gnueabihf}
RF=${3:--C target-cpu=cortex-a53}
ROOT=$HOME/src/brotli-bench
OUT=/tmp/rv-arm
CFLAGS="-O3 -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mthumb -g -DNDEBUG"
mkdir -p $OUT
cd $ROOT
UPPER0=$(echo $RT | tr 'a-z-' 'A-Z_')
if [ -n "${LOCAL:-}" ] && [ -z "${SKIP_RUST:-}" ]; then
  env CARGO_TARGET_${UPPER0}_LINKER=/bin/true CARGO_PROFILE_RELEASE_LTO=fat CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \
    CARGO_PROFILE_RELEASE_PANIC=abort RUSTFLAGS="$RF" cargo build -q -Zbuild-std=std,panic_abort \
    --manifest-path rust/Cargo.toml --release --target $RT --target-dir /tmp/rv-bstd 2>&1 | grep -E "^error" -A10 || true
  cp /tmp/rv-bstd/$RT/release/libbrotli_bench_ffi.a $OUT/libffi_$TAG.a
  rm -f vendor/rust-brotli-decompressor/Cargo.lock
  SKIP_RUST=1
fi
[ -z "${SKIP_RUST:-}" ] && rsync -a --delete --exclude target/ vendor/rust-brotli-decompressor/ linux:src/brotli-bench-rv/vendor/rust-brotli-decompressor/
if [ -z "${SKIP_RUST:-}" ]; then
rsync -a rust/ linux:src/brotli-bench-rv/rust/
UPPER=$(echo $RT | tr 'a-z-' 'A-Z_')
ssh linux "cd ~/src/brotli-bench-rv && ~/.local/bin/hostlock shared --label 'brotli-bench-rv: arm rust build' -- env \
  CARGO_TARGET_${UPPER}_LINKER=/bin/true CARGO_PROFILE_RELEASE_LTO=fat CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \
  CARGO_PROFILE_RELEASE_PANIC=abort RUSTFLAGS='$RF' ~/.cargo/bin/cargo build -q --manifest-path rust/Cargo.toml \
  --release --target $RT --target-dir /tmp/rv-cargo-$RT"
fi
[ -z "${SKIP_RUST:-}" ] && scp -q linux:/tmp/rv-cargo-$RT/$RT/release/libbrotli_bench_ffi.a $OUT/libffi_$TAG.a
if [ ! -f $OUT/libbrotli_c_prefixed.a ]; then
  rm -rf $OUT/objs && mkdir -p $OUT/objs
  for f in vendor/brotli-c/c/common/*.c vendor/brotli-c/c/dec/*.c vendor/brotli-c/c/enc/*.c; do
    d=$(basename $(dirname $f)); b=$(basename $f .c)
    arm-linux-gnueabihf-gcc $CFLAGS -Ivendor/brotli-c/c/include -c $f -o $OUT/objs/${d}_$b.o
  done
  arm-linux-gnueabihf-ar rcs $OUT/libbrotli_c.a $OUT/objs/*.o
  arm-linux-gnueabihf-nm -g --defined-only $OUT/libbrotli_c.a | awk 'NF == 3 && $2 !~ /^[Uu]$/ { print $3 " C_" $3 }' | sort -u > $OUT/c_syms.map
  arm-linux-gnueabihf-objcopy --redefine-syms=$OUT/c_syms.map $OUT/libbrotli_c.a $OUT/libbrotli_c_prefixed.a
  arm-linux-gnueabihf-g++ -std=c++17 $CFLAGS -Isrc -Ivendor/brotli-c/c/include src/bench_main.cc \
    $OUT/libbrotli_c.a $OUT/libbrotli_c_prefixed.a -static -pthread -lm -o $OUT/bench_c_rv
fi
arm-linux-gnueabihf-g++ -std=c++17 $CFLAGS -Wno-psabi -DBROTLI_BENCH_STANDARD_API_IS_RUST=1 -Isrc -Ivendor/brotli-c/c/include src/compare_main.cc \
  $OUT/libffi_$TAG.a $OUT/libbrotli_c_prefixed.a -static -pthread -ldl -lm -o $OUT/brotli_compare_$TAG
arm-linux-gnueabihf-g++ -std=c++17 $CFLAGS -Wno-psabi -DBROTLI_BENCH_STANDARD_API_IS_RUST=1 -Isrc -Ivendor/brotli-c/c/include src/bench_main.cc \
  $OUT/libffi_$TAG.a $OUT/libbrotli_c_prefixed.a -static -pthread -ldl -lm -o $OUT/bench_rust_$TAG
ssh linux mkdir -p /tmp/rv-arm
scp -q $OUT/brotli_compare_$TAG $OUT/bench_rust_$TAG $OUT/bench_c_rv linux:/tmp/rv-arm/
ssh linux "adb push /tmp/rv-arm/brotli_compare_$TAG /tmp/rv-arm/bench_rust_$TAG /tmp/rv-arm/bench_c_rv /data/local/tmp/brotli-bench/ >/dev/null"
echo built $TAG
