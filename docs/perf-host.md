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
| Frequency | Default: `powersave` governor (intel_pstate, EPP `balance_performance`), turbo on, base 3.5 GHz. For measurements, lock it with the tuner (see *Noise control* below) |
| Counters | Hardware PMU events available (`perf stat -e instructions:u,cycles:u` works; `perf_event_paranoid` = -1) |
| Toolchain | gcc 15.2, clang, `perf`, python3, git. **Rust: use the host's stable rustc/cargo 1.99.1** (`~/.cargo/bin`, decided 2026-10-30). CMake's `find_program` checks `~/.cargo/bin`, so no override is needed. The dev VM builds with Chromium's nightly toolchain, so numbers from the two machines are never compared. Record `rustc --version` with every result. `cmake`: Ubuntu package `cmake` (4.2.3), installed via apt under a `hostlock shared` hold (see the log in AGENTS.md). |
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
  3. still holding the lock, waits until the **1-minute load average is
     below 0.5 or the 5-minute one is below 1.0**, whichever comes first.
     `HOSTLOCK_QUIET_LOAD1` / `HOSTLOCK_QUIET_LOAD5` set the thresholds; 0
     turns a condition off; both 0, or the legacy `HOSTLOCK_QUIET_LOAD15=0`,
     skips the wait. Don't disable it casually;
  4. runs the command, then reopens the gate.

  After a build, expect a short "quieting" wait. (Until 2026-10-30 the rule
  used the 15-minute load average and could take much longer.)
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

## Noise control: `tune_benchmark_host.py`

The `optimize-campaign` skill ships a session-scoped host tuner:
`~/src/ai-skills/optimize-campaign/scripts/tune_benchmark_host.py`. It is on
the host, and its docs are in `references/measurement-policy.md` ("Host
policy inside a session"). For the duration of one command it applies:

- **CPU frequency:** turbo off (`intel_pstate/no_turbo=1`), `performance`
  governor and EPP, and every policy pinned to **min = max = base frequency**
  (3.5 GHz here).
- **NMI watchdog** off.
- **SMT** off by default (`smt/control=off`), so sibling CPUs 8-15 go offline
  and can't interfere. `--keep-smt` leaves SMT on.
- **ASLR** off by default. **Use `--keep-aslr` for timing runs**, as the
  campaign's score runs do: per-process layout randomization averages out
  alignment luck. Leave ASLR off only when stable addresses are needed (e.g.
  comparing `perf annotate` output across runs).
- VT/GPU/service options exist for browser campaigns; they aren't needed
  here.

How it behaves:
- Every setting is read back after writing.
- Everything is restored in reverse order when the command exits, even on
  SIGINT/SIGTERM.
- It uses `sudo -n` (passwordless sudo is configured on the host).
- It records the saved state in `/tmp/bench_host_tuning_state.json` and
  refuses to start if that file already exists.
  - If a session ever dies without restoring, run
    `tune_benchmark_host.py disable` to restore from that file.
  - Never delete the file by hand.
- `run` also takes the measurement lease. Inside a `hostlock exclusive` hold
  it reuses that hold (`HOSTLOCK_HELD`) instead of re-locking.
- `--report FILE` writes the before/during/after sysfs state plus observations
  (current frequencies, thermal throttle counters, load, online CPUs). Keep it
  next to the CSV; a throttle-count increase invalidates a run.

Canonical measurement command (the outer `hostlock` gives the hold a
descriptive label):

```bash
T=~/src/ai-skills/optimize-campaign/scripts/tune_benchmark_host.py
hostlock exclusive --label "brotli-bench: decode A/B" -- \
  python3 $T run --keep-aslr --report /tmp/tune-report.json -- \
    taskset -c 2 ./build/brotli_compare --bench --bench-csv /tmp/ab.csv
```

Absolute numbers at the locked 3.5 GHz base clock are lower than with turbo,
and they aren't comparable to the GCE baseline. Compare Rust/C ratios and
A/B deltas within the same tuned configuration only.

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
4. Measure under one exclusive hold *and* one tuner session (canonical
   command above), pinned to a single core (`taskset -c 2`; SMT is off
   during the session). Run the baseline build and the candidate
   back-to-back inside the same session, and keep the `--report` file.
5. **This host needs its own reference numbers.** Before the first decoder
   change, record a dated baseline here (e.g.
   `docs/baseline/bench-<date>-linux.csv`, plus an environment file). Add it
   as a new section in `baseline.md` per the AGENTS.md doc rules, then
   compare only within this host.
