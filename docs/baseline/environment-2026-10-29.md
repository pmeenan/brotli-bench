# Baseline environment (2026-10-29)

## Commits
brotli-bench: 2bc7431b3f3e39bc8972d9f9368d70d00c6f1772 (+ uncommitted scaffolding)
 d89c4b1c52e25ddafe047bc78bbee5c1e24b1809 vendor/brotli-c (go/brotli/v1.1.1-rc0-302-gd89c4b1)
 6df909c633c3860ea4bede0674798db8d88729e8 vendor/rust-brotli (heads/master)
 2ed02edcb8c35dc2eaa14ee5004fac9473b98bd6 vendor/rust-brotli-decompressor (heads/master)

## Toolchain
gcc (Debian 15.2.0-14) 15.2.0
rustc 1.101.0-nightly (4d020d894 2099-01-01) (8a9dd7abdf013ec10cfeaaa3685feba18ef1c90c-1-llvmorg-24-init-14453-ge063be79 chromium)
cargo 1.101.0-nightly (3960fa69b 2026-10-12) (8a9dd7abdf013ec10cfeaaa3685feba18ef1c90c-1-llvmorg-24-init-14453-ge063be79 chromium)
cmake version 3.31.12

## Build
C: -O3 -march=native -g (CMAKE_BUILD_TYPE=Release)
Rust: RUSTFLAGS='-C target-cpu=native', opt-level=3, lto=fat, codegen-units=1, panic=abort, debug=true

## Machine
Linux 7.1.6-1rodete1-amd64 x86_64
Architecture:                            x86_64
CPU op-mode(s):                          32-bit, 64-bit
Address sizes:                           46 bits physical, 48 bits virtual
Byte Order:                              Little Endian
CPU(s):                                  96
On-line CPU(s) list:                     0-95
Vendor ID:                               GenuineIntel
Model name:                              Intel(R) Xeon(R) CPU @ 2.00GHz
CPU family:                              6
Model:                                   85
Thread(s) per core:                      2
Core(s) per socket:                      24
Socket(s):                               2
Stepping:                                3
Microcode version:                       0xffffffff
BogoMIPS:                                3999.99
Virtualization:                          VT-x
Hypervisor vendor:                       KVM
Virtualization type:                     full
L1d cache:                               1.5 MiB (48 instances)
L1i cache:                               1.5 MiB (48 instances)
L2 cache:                                48 MiB (48 instances)
L3 cache:                                77 MiB (2 instances)
NUMA node(s):                            2
NUMA node0 CPU(s):                       0-23,48-71
NUMA node1 CPU(s):                       24-47,72-95

Flags (subset): fma sse4_2 popcnt avx abm bmi1 avx2 bmi2 avx512f avx512bw avx512vl 
