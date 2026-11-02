#!/bin/bash
# Runs the decompressor's test suite for 32-bit ARM under qemu-user, so the
# 32-bit (`reg_t = u32`) bit reader is covered; x86-64 and aarch64 test runs
# only build the 64-bit one.
#
# Needs a rustup toolchain with the `armv7-unknown-linux-musleabihf` target
# (`rustup target add armv7-unknown-linux-musleabihf`) and `qemu-arm` (the
# `linux` perf host has both). The musl target links with its self-contained
# CRT and `rust-lld`, so no ARM cross gcc is needed. Run it under
# `hostlock shared` on the perf host.
#
# Usage: scripts/test_armv7.sh [extra cargo test args, e.g. --release]
set -euo pipefail
cd "$(dirname "$0")/../vendor/rust-brotli-decompressor"
target_dir="${CARGO_TARGET_DIR:-/tmp/brotli-bench-armv7-test}"
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_LINKER=rust-lld
export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_RUNNER=qemu-arm
export RUSTFLAGS="-C link-self-contained=yes"
# qemu-arm user-mode emulation can race mmap/munmap across concurrent musl
# threads (occasionally failing libtest stdout writes with EFAULT).
export RUST_TEST_THREADS="${RUST_TEST_THREADS:-1}"
status=0
cargo test --target armv7-unknown-linux-musleabihf --target-dir "$target_dir" \
  --no-fail-fast "$@" || status=$?
# cargo creates an untracked lock file in the submodule.
rm -f Cargo.lock
exit $status
