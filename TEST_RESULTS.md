# Test results and status

## Status for the current worktree

Step 11–12 implementation is in progress and unverified against the current
worktree. The automation covers the listed core paths, but this is not a claim
that every requested edge case or measurement has been implemented. In
particular, QEMU/GDB evidence, per-workload memory and transaction-set
measurements, and lock wait-time/critical-section timing remain outstanding.
No all-suite, crash-recovery, or benchmark pass is claimed here.

An intermediate QEMU session during implementation did observe the following
before stopping:

| Case | Observed result | Scope |
|---|---|---|
| `xspec` | PASS marker printed | Intermediate source version; included lifecycle, namespace, conflict, allocation, and context-reuse checks. |
| `xnested` | PASS marker printed | Intermediate source version. |
| `xio console-abort` | FAIL/timeout after guest trap 14 (`err 7`) | Intermediate run. The complete I/O suite did not run. |
| Remaining I/O cases, stock `usertests` | NOT TESTED | Run stopped at the console-abort case. |
| Journal crash matrix (points 0–8) | NOT TESTED | Harness implemented; not executed. |
| Benchmarks and statistical summaries | NOT TESTED | Harness implemented; no current-worktree measurements. |

These observations are debugging evidence only. They are not final results for
the current worktree, and the I/O failure remains to be investigated during
the requested verification/fix phase.

## Reproducing the final run

From the repository root:

```sh
tests/run-tests.sh /path/to/qemu-system-i386
tests/crash-recovery.sh /path/to/qemu-system-i386
TRIALS=3 tests/run-benchmarks.sh /path/to/qemu-system-i386
```

`tests/run-all.sh /path/to/qemu-system-i386` runs all three commands. The
functional runner performs a clean kernel/filesystem build and runs each case
at both `CPUS=1` and `CPUS=3` by default. Override with `CPU_COUNTS="1 3"`.
Its CSV records CPU count and reports each case as `PASS`, `FAIL`, `BLOCKED`,
or `NOT_TESTED`; an unexpected QEMU exit,
panic/failure marker, timeout, or missing success marker returns nonzero.

## Required final results

The following are generated only by actually running the commands above:

- `results/tests/test-summary.csv` and `results/tests/test-summary.txt`
- `results/logs/functional-build.log` and
  `results/logs/functional-tests-cpu*.log`
- `results/recovery/crash-summary.csv` and per-point logs
- Each recovery log includes directory/hard-link checks and the host-side
  `tests/check-fs-image.py` inode-reference and allocation-bitmap result.
- `results/benchmarks/xbench-raw.csv`, trial logs, environment metadata,
  summary statistics, and SVG graphs

Do not replace missing results with estimates. A case that cannot be executed
must remain blocked or not tested with the reason recorded.
