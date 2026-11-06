# xv6-x86 baseline artifacts

`xv6-public/baseline.c` is the guest test and benchmark program. It reports CSV rows on the QEMU console. Rebuild and run it from the repository root with:

```sh
make -C xv6-public fs.img xv6.img
make -C xv6-public CPUS=1 qemu-nox
```

At the xv6 shell, enter `baseline`. Repeat with `CPUS=3` to exercise the concurrent writer test on multiple CPUs. The test removes its temporary files. Copy the emitted rows into `xv6-x86-baseline.csv`; console output and test notes are recorded in `xv6-x86-baseline.log`.

The CSV currently contains one trial at each CPU count. `uptime()` has tick-level resolution, so zero ticks for small writes is expected and these values are not a statistically useful performance comparison. The test verifies ordinary file create/read/unlink, console output, a forked pipe transfer, concurrent child writes to a shared file, and successful writes from 1 KB through 64 KB.

The lifecycle test is the `xspec` guest program. Current source tests shadow-block isolation, commit publication, abort cleanup and fail-closed mutation handling. The checked-in lifecycle log predates the Step 6 publisher and is historical evidence only; use `TEST_RESULTS.md` for commands and revision-qualified results.
