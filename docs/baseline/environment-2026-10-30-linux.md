# Perf host environment (2026-10-30, linux)

## Commits
brotli-bench: d0dfa29fb18d2320ea111a3cec07beb6608a9936 (synced working tree, not pushed)
 d89c4b1c52e25ddafe047bc78bbee5c1e24b1809 vendor/brotli-c (go/brotli/v1.1.1-rc0-302-gd89c4b1)
 27a185942d0ba4b01681aa988b9c78e6512faccd vendor/rust-brotli (9.1.1-9-g27a1859)
 2ed02edcb8c35dc2eaa14ee5004fac9473b98bd6 vendor/rust-brotli-decompressor (6.2.0-6-g2ed02ed)

## Toolchain
gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
rustc 1.99.1 (58cf8f9c3 2026-10-13)
cargo 1.99.1 (5f94df478 2026-08-27)
cmake version 4.2.3

## Build
C: -O3 -march=native -g (CMAKE_BUILD_TYPE=Release)
Rust: RUSTFLAGS="-C target-cpu=native", opt-level=3, lto=fat, codegen-units=1, panic=abort, debug=true

## Measurement session
hostlock exclusive + tune_benchmark_host.py run --keep-aslr (turbo off, performance governor/EPP, min=max=3.5 GHz base, NMI watchdog off, SMT off -> CPUs 0-7 online, ASLR on); taskset -c 2. Full report: tune-report-2026-10-30-linux.json.

## Host state during the session
```
# start 2026-10-29T21:43:31-04:00
loadavg: 0.79 0.98 0.99 1/1442 1259548
online: 0-7
no_turbo: 1
governors:       8 performance 
min/max: 3500000 / 3500000
cur freq (cpu2): 3500000
throttle counts:
cpu0/thermal_throttle/core_throttle_count:0
cpu0/thermal_throttle/package_throttle_count:0
cpu1/thermal_throttle/core_throttle_count:0
cpu1/thermal_throttle/package_throttle_count:0
cpu2/thermal_throttle/core_throttle_count:0
cpu2/thermal_throttle/package_throttle_count:0
cpu3/thermal_throttle/core_throttle_count:0
cpu3/thermal_throttle/package_throttle_count:0
cpu4/thermal_throttle/core_throttle_count:0
cpu4/thermal_throttle/package_throttle_count:0
cpu5/thermal_throttle/core_throttle_count:0
cpu5/thermal_throttle/package_throttle_count:0
cpu6/thermal_throttle/core_throttle_count:0
cpu6/thermal_throttle/package_throttle_count:0
cpu7/thermal_throttle/core_throttle_count:0
cpu7/thermal_throttle/package_throttle_count:0
# after-bench 2026-10-29T22:00:49-04:00
loadavg: 1.15 1.08 1.01 3/1430 1264794
online: 0-7
no_turbo: 1
governors:       8 performance 
min/max: 3500000 / 3500000
cur freq (cpu2): 3500000
throttle counts:
cpu0/thermal_throttle/core_throttle_count:0
cpu0/thermal_throttle/package_throttle_count:0
cpu1/thermal_throttle/core_throttle_count:0
cpu1/thermal_throttle/package_throttle_count:0
cpu2/thermal_throttle/core_throttle_count:0
cpu2/thermal_throttle/package_throttle_count:0
cpu3/thermal_throttle/core_throttle_count:0
cpu3/thermal_throttle/package_throttle_count:0
cpu4/thermal_throttle/core_throttle_count:0
cpu4/thermal_throttle/package_throttle_count:0
cpu5/thermal_throttle/core_throttle_count:0
cpu5/thermal_throttle/package_throttle_count:0
cpu6/thermal_throttle/core_throttle_count:0
cpu6/thermal_throttle/package_throttle_count:0
cpu7/thermal_throttle/core_throttle_count:0
cpu7/thermal_throttle/package_throttle_count:0
# end 2026-10-29T22:01:01-04:00
loadavg: 1.13 1.07 1.01 2/1445 1264987
online: 0-7
no_turbo: 1
governors:       8 performance 
min/max: 3500000 / 3500000
cur freq (cpu2): 3500000
throttle counts:
cpu0/thermal_throttle/core_throttle_count:0
cpu0/thermal_throttle/package_throttle_count:0
cpu1/thermal_throttle/core_throttle_count:0
cpu1/thermal_throttle/package_throttle_count:0
cpu2/thermal_throttle/core_throttle_count:0
cpu2/thermal_throttle/package_throttle_count:0
cpu3/thermal_throttle/core_throttle_count:0
cpu3/thermal_throttle/package_throttle_count:0
cpu4/thermal_throttle/core_throttle_count:0
cpu4/thermal_throttle/package_throttle_count:0
cpu5/thermal_throttle/core_throttle_count:0
cpu5/thermal_throttle/package_throttle_count:0
cpu6/thermal_throttle/core_throttle_count:0
cpu6/thermal_throttle/package_throttle_count:0
cpu7/thermal_throttle/core_throttle_count:0
cpu7/thermal_throttle/package_throttle_count:0
```

## Machine
Linux 7.0.0-42-generic x86_64
Architecture:                            x86_64
CPU op-mode(s):                          32-bit, 64-bit
Address sizes:                           39 bits physical, 48 bits virtual
Byte Order:                              Little Endian
CPU(s):                                  16
On-line CPU(s) list:                     0-15
Vendor ID:                               GenuineIntel
Model name:                              11th Gen Intel(R) Core(TM) i9-11900KF @ 3.50GHz
CPU family:                              6
Model:                                   167
Thread(s) per core:                      2
Core(s) per socket:                      8
Socket(s):                               1
Stepping:                                1
CPU(s) scaling MHz:                      79%
CPU max MHz:                             5300.0000
CPU min MHz:                             800.0000
BogoMIPS:                                7008.00
Virtualization:                          VT-x
L1d cache:                               384 KiB (8 instances)
L1i cache:                               256 KiB (8 instances)
L2 cache:                                4 MiB (8 instances)
L3 cache:                                16 MiB (1 instance)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-15

Flags (subset): fma sse4_2 popcnt avx abm bmi1 avx2 bmi2 avx512f avx512bw avx512vl 

MemTotal:       64615256 kB
THP: always [madvise] never
core_throttle_count
core_throttle_max_time_ms
core_throttle_total_time_ms
