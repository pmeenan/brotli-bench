#!/bin/bash
# Usage: scripts/android/bench.sh TAG OUTNAME [extra brotli_compare args]
# Runs a decode-only bench subset on Android cpu2, Rust direct API.
TAG=$1; NAME=$2; shift 2
ssh linux "adb shell 'cd /data/local/tmp/brotli-bench && taskset 4 ./brotli_compare_$TAG --bench --decode-only --rust-api direct --min-time-ms 400 --bench-csv /data/local/tmp/brotli-bench/rv-$NAME.csv $* > /data/local/tmp/brotli-bench/rv-$NAME.log 2>&1; echo rc=\$?' && adb pull /data/local/tmp/brotli-bench/rv-$NAME.csv /tmp/rv-arm/ >/dev/null && adb pull /data/local/tmp/brotli-bench/rv-$NAME.log /tmp/rv-arm/ >/dev/null"
scp -q linux:/tmp/rv-arm/rv-$NAME.csv linux:/tmp/rv-arm/rv-$NAME.log /tmp/rv-arm/
