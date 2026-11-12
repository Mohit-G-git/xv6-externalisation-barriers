# Performance benchmark report

## Implementation status

The Step 12 workload and report-generation infrastructure is implemented.
Benchmark execution and measurement collection are pending; therefore this
document intentionally contains no fabricated timings, summary statistics,
or graphs.

## Method

`xv6-public/xbench.c` measures ordinary and speculative filesystem
create/write/read/unlink batches, directory operations, commit and abort
overheads, nested commit/abort, console output, pipe communication,
same-context process workloads, independent contexts, same-block conflicts,
different-file concurrency, and shadow capacity. The write-size sweep measures
1, 2, 4, 8, 16, 32, and 64 KiB writes; it reports ordinary and speculative
write time separately from speculative commit and abort-cleanup time.
Workloads emit CSV rows
including the workload, mode, participant count, write-set blocks, iteration
count, bytes, elapsed xv6 ticks, and status. At the end of the run the guest
prints `SHADOW_METRICS` with current and peak occupancy, capacity, allocation
failures, lookup totals/hits/misses, bucket-lock attempts/contention, and
per-entry data-lock acquisitions. Lookup counters include validation and
snapshot table lookups; bucket contention is an observed-busy-at-sample
counter, not a time-based lock-wait measurement.

The empty-transaction commit/abort rows separate lifecycle-only batches from
the write-set batches; write-set sweeps report combined begin/write/validation/
publication or begin/write/abort time. This x86 xv6 timer does not provide
sub-tick syscall timings, so the report must not present those batch timings as
isolated per-call phase costs.

`tests/run-benchmarks.sh` builds cleanly for both the fine bucket-lock and
coarse shadow-metadata-lock modes, copies a clean filesystem image for every
repetition, runs identical workloads at one and three QEMU CPUs by default,
and appends trial, CPU count, lock strategy, Git SHA, process limit, and kernel
configuration to each raw row. Set `TRIALS` to choose repetitions (default: 3),
`CPU_COUNTS` to change the CPU matrix, or `LOCK_STRATEGIES` to change the
strategy matrix. The environment record includes host, QEMU, compiler, binary
path, Git SHA, CPU counts, lock strategies, process limit, build command, and
timer source. The raw per-trial data is kept at
`results/benchmarks/xbench-raw.csv`; the accompanying
`results/benchmarks/shadow-metrics.csv` contains the storage and locking
counters for each repetition. `trial-summary.csv` distinguishes passed,
failed, blocked, and not-run repetitions.

The guest timer is xv6 `uptime()`, whose coarse resolution means small
measurements can report zero ticks. Such a result is not zero execution cost.
Each row records the batch elapsed ticks and workload operation count. The
host summary script calculates mean, median, sample standard deviation,
minimum, and maximum from successful trials only while leaving every trial
and status in the raw CSV.

## Reproducible outputs

```sh
TRIALS=3 tests/run-benchmarks.sh /path/to/qemu-system-i386
tests/summarize-benchmarks.py results/benchmarks/xbench-raw.csv results/benchmarks
tests/plot-benchmarks.py results/benchmarks/xbench-raw.csv results/benchmarks/graphs
```

The graph generator reads the raw file; it does not embed measurements. It
generates SVG comparisons for ordinary/speculative batches, commit/abort by
write-set size, write-size latency and successful throughput, and shared
versus independent contexts by participant count. Each chart is separated
by CPU count and lock strategy; capacity-limited rows remain in CSV but are
excluded from successful-latency charts.
Graphs are only produced for workloads with successful recorded rows.

## Interpretation limits

- A previous baseline CSV is retained under `results/`, but it came from an
  older source revision and is not directly comparable to measurements from
  this worktree.
- QEMU scheduling and coarse xv6 ticks may dominate short operations.
- Conflict rows are retained and are not included in successful-latency
  statistics.
- Lock counters report acquisitions and observed-busy samples, not elapsed
  wait time or critical-section duration. The coarse baseline serializes
  shadow metadata through one bucket; it is not a global filesystem lock.
- Shadow occupancy and allocation statistics are guest-run aggregates, not
  sampled per transaction or process. Process-scaling workloads top out at 32
  participants in this configuration; the 64-entry shadow table and 16-entry
  per-transaction quota constrain successful write sets.
- Console and pipe workloads exercise fixed-size cases, not a full output-size
  sweep. The current artifacts do not contain memory-consumption graphs or
  separate read-set/validation-cost curves.
- Bounded guest stress is empirical evidence, not proof that races or
  deadlocks are impossible.
- No parallel speedup, scalability improvement, or general performance
  advantage is claimed without comparable successful raw trials.

Populate a final comparison and conclusions only after executing the
reproduction commands and reviewing the retained CSVs and logs.
