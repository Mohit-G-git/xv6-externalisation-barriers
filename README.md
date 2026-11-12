# Externalization Barriers for Speculative Processes

CSL3030 Operating Systems Lab — Project 6. Platform: xv6-public x86.

This project adds kernel-managed speculative execution to xv6. A process can
start a speculation with `xbegin()`, publish it with `xcommit()`, or discard
it with `xabort()`. Speculative filesystem changes are isolated from unrelated
processes until commit; a process and its forked descendants share a
read-your-own-writes view. Nested speculations merge into their parent, and
only the outermost commit publishes to the filesystem. Console and pipe output
are buffered according to speculation membership.

The implementation uses private shadow blocks and xv6's redo journal for
filesystem publication, with optimistic version validation and an
abort-on-conflict policy. Shadow storage and the per-context write set are
bounded; a write or commit that exceeds capacity must be reported as a
failure, not treated as a successful low-latency operation. The durable journal
commit point and delivery of buffered console/pipe output are distinct events.
This project does not implement durable multi-version snapshots, and output
delivery cannot be made crash-atomic with disk state.

The accompanying project document defines a 12-step development roadmap.
Architecture and threat-model notes are in [ARCHITECTURE.md](./ARCHITECTURE.md)
and [THREAT_MODEL.md](./THREAT_MODEL.md). The current test and benchmark
coverage is not a claim that every roadmap item is complete: the full
verification run has not passed on the current worktree. Known gaps and
intermediate failure evidence are recorded in [TEST_RESULTS.md](./TEST_RESULTS.md)
and [BENCHMARK_REPORT.md](./BENCHMARK_REPORT.md).

## Requirements

The host needs:

- `make`, GCC and the 32-bit GCC/binutils support required by xv6 x86
- Perl (used by the xv6 source-generation scripts)
- QEMU with `qemu-system-i386`
- Expect
- Python 3 (filesystem consistency and benchmark reporting scripts)
- Standard shell utilities including `awk`, `grep`, and `sed`

For Debian/Kali, the usual packages are:

```sh
sudo apt install build-essential gcc-multilib expect qemu-system-x86 python3 perl
```

If QEMU is not on `PATH`, provide its absolute path in `QEMU` or as the first
argument to a test runner.

## Build and launch xv6

From the repository root, set the QEMU path once:

```sh
export QEMU=/absolute/path/to/qemu-system-i386
```

Build the kernel and filesystem images:

```sh
cd xv6-public
make clean
make -j2 QEMU="$QEMU" fs.img xv6.img
```

Launch the interactive guest with two CPUs (the default):

```sh
make QEMU="$QEMU" CPUS=2 qemu-nox
```

Exit QEMU with `Ctrl-A`, then `X`. The same build can be started directly with
`make QEMU="$QEMU" CPUS=1 qemu-nox` or `CPUS=3`.

## Run the complete automated workflow

Return to the repository root and run all three suites:

```sh
cd ..
TRIALS=3 CPU_COUNTS="1 3" LOCK_STRATEGIES="fine coarse" \
  tests/run-all.sh "$QEMU"
```

This runs the functional/regression suite, the nine-point journal
crash/recovery suite, and the repeated performance benchmarks in that order.
The benchmark matrix above means 3 repetitions for each combination of two
CPU counts and two shadow-lock modes. These workflows rebuild xv6 as needed,
so allow time for the crash matrix and benchmark repetitions. `run-all.sh`
continues to later suites if an earlier suite fails and exits nonzero if any
suite fails.

For easier diagnosis, run the stages separately:

```sh
tests/run-tests.sh "$QEMU"
tests/crash-recovery.sh "$QEMU"
TRIALS=3 CPU_COUNTS="1 3" LOCK_STRATEGIES="fine coarse" \
  tests/run-benchmarks.sh "$QEMU"
```

### Functional and regression tests

`tests/run-tests.sh` builds a clean filesystem/kernel, then runs the
speculation, nesting, exec/fork, console/pipe, and stock xv6 `usertests`
workloads under one and three QEMU CPUs by default. It records each expected
marker and test status. To run one case on both CPU counts:

```sh
TEST_FILTER=xspec tests/run-tests.sh "$QEMU"
```

Change the CPU matrix with `CPU_COUNTS`, for example:

```sh
CPU_COUNTS="1 3" tests/run-tests.sh "$QEMU"
```

### Crash recovery

`tests/crash-recovery.sh` injects crashes at journal/commit stages 0–8. For
each stage, it boots a copied `fs.img`, crashes the guest, and boots again
against that same image (it rebuilds the kernel, not the disk image, between
the crash and recovery boots). Recovery checks verify expected committed or
discarded file and directory state, hard-link identity/link counts, and
filesystem usability. `tests/check-fs-image.py` checks inode references and
allocation-bitmap consistency after recovery. Results and raw serial logs are
written under `results/recovery/`.

### Benchmarks

`tests/run-benchmarks.sh` compares the fine bucket-lock build and coarse
shadow-metadata-lock build at one and three CPUs. It repeats ordinary and
speculative workloads, includes 1, 2, 4, 8, 16, 32, and 64 KiB write sizes,
and records write, commit, and abort-cleanup phases separately where
measurable. It preserves failed/conflicting/capacity-limited rows rather than
counting them as successes. Raw data, environment/build metadata, trial logs,
summary statistics, and SVG graphs are written under `results/benchmarks/`.

Useful settings:

```sh
TRIALS=5 tests/run-benchmarks.sh "$QEMU"
CPU_COUNTS="1 3" tests/run-benchmarks.sh "$QEMU"
LOCK_STRATEGIES="fine coarse" tests/run-benchmarks.sh "$QEMU"
```

To regenerate summaries and graphs from existing raw data:

```sh
tests/summarize-benchmarks.py \
  results/benchmarks/xbench-raw.csv results/benchmarks
tests/plot-benchmarks.py \
  results/benchmarks/xbench-raw.csv results/benchmarks/graphs
```

Measurements use xv6 `uptime()` ticks and can be too coarse to resolve short
operations. Lock counters report acquisitions and observed contention, not
elapsed lock-wait time or critical-section duration. Process workloads are
bounded by the xv6 process and shadow-table limits. See
[BENCHMARK_REPORT.md](./BENCHMARK_REPORT.md) for interpretation limitations;
do not infer performance improvement without comparable successful raw trials.

## Generated evidence

- Functional test CSV/text summaries: `results/tests/`
- Functional build and per-CPU serial logs: `results/logs/`
- Crash/recovery CSV, environment record, and logs: `results/recovery/`
- Benchmark raw CSVs, metadata, summaries, graphs, and logs:
  `results/benchmarks/`

Only report results produced by actually running the corresponding workflow.
The checked-in documentation distinguishes implementation coverage from
verified behavior; current intermediate failures and untested cases are
described in [TEST_RESULTS.md](./TEST_RESULTS.md).
