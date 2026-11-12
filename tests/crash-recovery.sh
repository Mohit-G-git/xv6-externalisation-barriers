#!/bin/sh
set -eu

qemu=${1:-qemu-system-i386}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel_dir="$root/xv6-public"
results="$root/results/recovery"
logs="$results/logs"
summary="$results/crash-summary.csv"

blocked_report()
{
  mkdir -p "$logs"
  printf 'point,expected,actual,status,crash_log,recovery_log\n' > "$summary"
  for point in 0 1 2 3 4 5 6 7 8; do
    printf '%s,crash/recovery,dependency unavailable,BLOCKED,,\n' "$point" \
      >> "$summary"
  done
  {
    echo "Crash-recovery summary"
    echo "Total points: 9"
    echo "Passed: 0"
    echo "Failed: 0"
    echo "Blocked: 9"
    echo "Not tested: 0"
    echo "Reason: required host dependency unavailable"
  } > "$results/crash-summary.txt"
}

for tool in make gcc expect python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required tool not found: $tool" >&2
    blocked_report
    exit 2
  fi
done
if ! command -v "$qemu" >/dev/null 2>&1 && [ ! -x "$qemu" ]; then
  echo "QEMU binary not found or not executable: $qemu" >&2
  blocked_report
  exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/xv6-crash-test.XXXXXX")
cleanup()
{
  if [ -f "$summary" ]; then
    for point in 0 1 2 3 4 5 6 7 8; do
      if ! grep -q "^$point," "$summary"; then
        printf '%s,unknown,not-run,NOT_TESTED,,\n' "$point" >> "$summary"
      fi
    done
    awk -F, 'NR > 1 { count++; status[$4]++ }
      END {
        printf "Crash-recovery summary\n";
        printf "Total points: %d\n", count;
        printf "Passed: %d\n", status["PASS"] + 0;
        printf "Failed: %d\n", status["FAIL"] + 0;
        printf "Not tested: %d\n", status["NOT_TESTED"] + 0;
        printf "Machine-readable results: results/recovery/crash-summary.csv\n";
        printf "Raw logs: results/recovery/logs/\n";
      }' "$summary" > "$results/crash-summary.txt"
  fi
  rm -f "$work/fs.img"
  rmdir "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$logs"
printf 'point,expected,actual,status,crash_log,recovery_log\n' > "$summary"
{
  echo "Host: $(uname -a)"
  echo "QEMU: $("$qemu" --version 2>&1 | sed -n '1p')"
  echo "Compiler: $(gcc --version 2>&1 | sed -n '1p')"
  echo "Injected points: 0 through 8"
  echo "Disk model: one copied fs.img reused across crash and recovery boots per point"
} > "$results/environment.txt"
cd "$kernel_dir"
for point in 0 1 2 3 4 5 6 7 8; do
  echo "Testing journal crash point $point"
  if ! make clean >"$logs/build-point-$point.log" 2>&1 ||
     ! make -j2 SPEC_CRASH_POINT="$point" fs.img xv6.img \
       >>"$logs/build-point-$point.log" 2>&1; then
    cat "$logs/build-point-$point.log" >&2
    printf '%s,unknown,unknown,FAIL,build-point-%s.log,\n' \
      "$point" "$point" >> "$summary"
    exit 1
  fi
  cp fs.img "$work/fs.img"

  crash_log="$logs/point-$point-crash.log"
  if ! expect "$root/tests/crash-point.exp" "$qemu" xv6.img "$work/fs.img" \
       "$point" >"$crash_log" 2>&1; then
    cat "$crash_log" >&2
    printf '%s,unknown,unknown,FAIL,point-%s-crash.log,\n' \
      "$point" "$point" >> "$summary"
    exit 1
  fi

  if [ "$point" -eq 0 ] || [ "$point" -eq 1 ] ||
     [ "$point" -eq 2 ] || [ "$point" -eq 8 ]; then
    expected=verify-absent
    marker="XCRASH RECOVERY ABSENT PASS"
  else
    expected=verify-present
    marker="XCRASH RECOVERY PRESENT PASS"
  fi
  recovery_log="$logs/point-$point-recovery.log"
  if ! make clean >"$logs/recovery-build-point-$point.log" 2>&1 ||
     ! make -j2 fs.img xv6.img >>"$logs/recovery-build-point-$point.log" 2>&1; then
    cat "$logs/recovery-build-point-$point.log" >&2
    printf '%s,%s,unknown,FAIL,point-%s-crash.log,recovery-build-point-%s.log\n' \
      "$point" "$expected" "$point" "$point" >> "$summary"
    exit 1
  fi
  if ! expect "$root/tests/recovery-check.exp" "$qemu" xv6.img "$work/fs.img" \
       "$expected" "$marker" >"$recovery_log" 2>&1; then
    cat "$recovery_log" >&2
    printf '%s,%s,unknown,FAIL,point-%s-crash.log,point-%s-recovery.log\n' \
      "$point" "$expected" "$point" "$point" >> "$summary"
    exit 1
  fi
  if ! python3 "$root/tests/check-fs-image.py" "$work/fs.img" \
       >>"$recovery_log" 2>&1; then
    cat "$recovery_log" >&2
    printf '%s,%s,filesystem-inconsistent,FAIL,point-%s-crash.log,point-%s-recovery.log\n' \
      "$point" "$expected" "$point" "$point" >> "$summary"
    exit 1
  fi
  if grep -q 'XCRASH_PENDING_CONSOLE' "$crash_log"; then
    echo "Buffered console output escaped before commit at crash point $point" >&2
    printf '%s,%s,console-leak,FAIL,point-%s-crash.log,point-%s-recovery.log\n' \
      "$point" "$expected" "$point" "$point" >> "$summary"
    exit 1
  fi
  printf '%s,%s,%s,PASS,point-%s-crash.log,point-%s-recovery.log\n' \
    "$point" "$expected" "$marker" "$point" "$point" >> "$summary"
done

echo "Crash recovery passed at all nine journal points. Summary: $summary"
