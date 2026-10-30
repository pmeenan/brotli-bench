# Performance test host (`linux`) and `hostlock`

Decoder (and later encoder) performance numbers should come from the physical
box reachable as `ssh linux`. The GCE dev VM (where the 2026-10-29 baseline
was taken) has no hardware performance counters and noisier encode timings.

## Machine (checked 2026-10-30)

| | |
|---|---|
| SSH | `ssh linux` (`home.meenan.us:10026`, user `pmeenan`, key `~/.ssh/id_ed25519`) |
| CPU | Intel Core i9-11900KF (Rocket Lake), 8 cores / 16 threads, 16 MiB L3, max turbo 5.1 GHz (5.3 GHz on cores 6-7) |
| SMT siblings | CPU *n* and *n*+8 share core *n* (e.g. 2 and 10) |
| Memory | 61 GiB; THP `madvise` |
| OS | Ubuntu, kernel 7.0 |
| Frequency | `powersave` governor (intel_pstate, EPP `balance_performance`), turbo on. Expect clock variance; always A/B back-to-back |
| Counters | Hardware PMU events available (`perf stat -e instructions:u,cycles:u` works; `perf_event_paranoid` = -1) |
| Toolchain | gcc 15.2, clang, `perf`, python3, git, stable rustc/cargo 1.99.1 (`~/.cargo/bin`, on the interactive `PATH`). **No `cmake`**: install it before building the harness (ask first). Note that the dev VM builds with Chromium's nightly toolchain; pick one toolchain per comparison and record it. |
| Shared | Other projects (e.g. `chunky`, Chromium campaigns) run builds and measurements here |

## Reserving the machine: `hostlock`

The host uses a machine-wide reader/writer lock: `~/.local/bin/hostlock`,
with lock files in `~/.hostlock/`. Its own documentation is
`~/.hostlock/README.md` and `hostlock --help` on the host. The rules:

- **Heavy work runs under a shared hold.** Heavy work means builds (cmake,
  cargo, ninja), test suites, `brotli_compare --verify`, or anything that
  keeps cores busy for more than a few seconds. Any number of shared holds
  run together.
  ```bash
  hostlock shared --label "brotli-bench: build" -- cmake --build build -j
  ```
- **Measurements take the exclusive hold.** This covers `brotli_compare
  --bench`, `bench_c` / `bench_rust` timing runs, and `perf record` / `perf
  stat` profiling meant for numbers.
  ```bash
  hostlock exclusive --label "brotli-bench: decode A/B" -- \
    taskset -c 2 ./build/brotli_compare --bench --bench-csv /tmp/ab.csv
  ```
  What the exclusive hold does:
  1. closes the gate, so no new shared job can start;
  2. waits for running shared jobs to finish;
  3. still holding the lock, waits until the **15-minute load average is
     below 1.0** (`HOSTLOCK_QUIET_LOAD15`; 0 disables; don't disable it
     casually);
  4. runs the command, then reopens the gate.

  After a build, expect several minutes in the "quieting" state.
- **Light work needs no lock:** editing, git, reading files, short scripts.
- **Status:**
  - `hostlock status` shows the holders, the waiters, anything quieting, and
    whether the gate is open.
  - `--timeout SEC` gives up with exit code 75.
- **Nesting:**
  - A holder exports `HOSTLOCK_HELD=<mode>:<pid>`, so child processes don't
    re-lock.
  - `exclusive` inside `shared` is refused, because it would deadlock.
  - Wrap a whole measurement session in a single `exclusive` (e.g. a script
    running all A/B cells) rather than many small ones.
- Locks are `flock(2)` locks, released when the process exits, even on a
  crash. **Never delete `~/.hostlock`.**

## Measurement recipe for this project

1. Sync the code: clone or fetch `pmeenan/brotli-bench` on the host. Never
   copy build outputs between machines.
2. Build under a shared hold (once `cmake` is available):
   ```bash
   hostlock shared --label "brotli-bench: build" -- \
     sh -c 'git submodule update --init && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j'
   ```
3. Run `--verify` under a shared hold (it's a correctness check, not a
   measurement).
4. Measure under one exclusive hold, pinned to a single core with its SMT
   sibling left idle (e.g. `taskset -c 2`, keep CPU 10 free). Run the
   baseline build and the candidate back-to-back inside the same hold.
5. **This host needs its own reference numbers.** Before the first decoder
   change, record a dated baseline here (e.g.
   `docs/baseline/bench-<date>-linux.csv`, plus an environment file). Add it
   as a new section in `baseline.md` per the AGENTS.md doc rules, then
   compare only within this host.
