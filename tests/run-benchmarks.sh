#!/bin/sh
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel_dir="$root/xv6-public"
results="$root/results/benchmarks"
logs="$results/logs"
qemu=${1:-${QEMU:-}}
trials=${TRIALS:-3}
cpu_counts=${CPU_COUNTS:-"1 3"}
lock_strategies=${LOCK_STRATEGIES:-"fine coarse"}
git_sha=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)

if [ -z "$qemu" ]; then
  qemu=$(command -v qemu-system-i386 || command -v qemu-system-x86_64 || true)
fi
if [ -z "$qemu" ] || { [ ! -x "$qemu" ] && ! command -v "$qemu" >/dev/null 2>&1; }; then
  echo "QEMU binary not found or not executable. Set QEMU or pass its path." >&2
  exit 2
fi
if ! command -v make >/dev/null 2>&1 ||
   ! command -v gcc >/dev/null 2>&1 ||
   ! command -v expect >/dev/null 2>&1 ||
   ! command -v awk >/dev/null 2>&1 ||
   ! command -v python3 >/dev/null 2>&1; then
  echo "Required tools: make, gcc, expect, awk, python3, and QEMU." >&2
  exit 2
fi
case "$trials" in
  ''|*[!0-9]*|0) echo "TRIALS must be a positive integer." >&2; exit 2 ;;
esac

mkdir -p "$logs"
raw="$results/xbench-raw.csv"
shadow_raw="$results/shadow-metrics.csv"
trial_summary="$results/trial-summary.csv"
trial_report="$results/trial-summary.txt"
printf 'trial,cpu_count,lock_strategy,expected,actual,status,log\n' > "$trial_summary"
base_fs=$(mktemp "${TMPDIR:-/tmp}/xv6-bench-fs.XXXXXX")
total_trials=0
finalize()
{
  trial=1
  while [ "$trial" -le "$total_trials" ]; do
    if ! grep -q "^$trial," "$trial_summary"; then
      printf '%s,,,"benchmark output","not run",NOT_TESTED,\n' "$trial" >> "$trial_summary"
    fi
    trial=$((trial + 1))
  done
  awk -F, 'NR > 1 { count++; status[$6]++ }
    END {
      printf "Benchmark trial summary\n";
      printf "Total configurations: %d\n", count;
      printf "Passed: %d\n", status["PASS"] + 0;
      printf "Failed: %d\n", status["FAIL"] + 0;
      printf "Blocked: %d\n", status["BLOCKED"] + 0;
      printf "Not tested: %d\n", status["NOT_TESTED"] + 0;
      printf "Raw data: results/benchmarks/xbench-raw.csv\n";
      printf "Trial log directory: results/benchmarks/logs/\n";
    }' "$trial_summary" > "$trial_report"
  rm -f "$base_fs"
}
trap finalize EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

{
  echo "Host: $(uname -a)"
  echo "QEMU: $("$qemu" --version 2>&1 | sed -n '1p')"
  echo "Compiler: $(gcc --version 2>&1 | sed -n '1p')"
  echo "QEMU binary: $qemu"
  echo "Git commit: $git_sha"
  echo "CPU counts: $cpu_counts"
  echo "Lock strategies: $lock_strategies"
  echo "Kernel configuration: xv6-public x86, NPROC=64, NCPU=8, NSHADOW=64, per-context shadow quota=16"
  echo "SPEC_LOCK_MODE: fine (bucket) or coarse (single shadow metadata bucket)"
  echo "Kernel directory: $kernel_dir"
  echo "Build: make clean && make -j2 QEMU=<qemu> fs.img xv6.img"
  echo "Workload: xbench, $trials independent repetitions"
  echo "Timer: xv6 uptime() ticks; timer interval is documented in trap.c"
} > "$results/environment.txt"

printf 'experiment,mode,processes,blocks,iterations,bytes,ticks,status,trial,log,cpu_count,lock_strategy,git_sha,process_limit,kernel_config\n' > "$raw"
printf 'trial,allocated,peak,capacity,allocation_failures,lookups,lookup_hits,lookup_misses,bucket_acquires,bucket_contention,data_acquires,log,cpu_count,lock_strategy,git_sha\n' > "$shadow_raw"

trial=0
for strategy in $lock_strategies; do
  case "$strategy" in
    fine|coarse) ;;
    *) echo "Unsupported lock strategy: $strategy (expected fine or coarse)" >&2; exit 2 ;;
  esac
  build_log="$logs/build-$strategy.log"
  if ! (cd "$kernel_dir" && make clean >"$build_log" 2>&1 &&
        make -j2 QEMU="$qemu" SPEC_LOCK_MODE="$strategy" fs.img xv6.img \
          >>"$build_log" 2>&1); then
    cat "$build_log" >&2
    exit 1
  fi
  cp "$kernel_dir/fs.img" "$base_fs"
  for cpu in $cpu_counts; do
    case "$cpu" in ''|*[!0-9]*|0) echo "Invalid CPU count: $cpu" >&2; exit 2 ;; esac
    repetition=1
    while [ "$repetition" -le "$trials" ]; do
      trial=$((trial + 1))
      total_trials=$trial
      trial_fs="$results/trial-$trial-fs.img"
      trial_log="$logs/trial-$trial.log"
      cp "$base_fs" "$trial_fs"
      if ! expect "$root/tests/benchmark.exp" "$qemu" "$kernel_dir/xv6.img" \
           "$trial_fs" "$cpu" >"$trial_log" 2>&1; then
        cat "$trial_log" >&2
        printf '%s,%s,%s,XBENCH DONE,guest/QEMU failed,FAIL,logs/trial-%s.log\n' \
          "$trial" "$cpu" "$strategy" "$trial" >> "$trial_summary"
        echo "Benchmark configuration $trial failed; see $trial_log" >&2
        exit 1
      fi
      if ! grep -q 'XBENCH DONE' "$trial_log"; then
        printf '%s,%s,%s,XBENCH DONE,marker missing,FAIL,logs/trial-%s.log\n' \
          "$trial" "$cpu" "$strategy" "$trial" >> "$trial_summary"
        echo "Benchmark completion marker missing in configuration $trial" >&2
        exit 1
      fi
      sed 's/\r$//' "$trial_log" |
        awk -F, -v trial="$trial" -v log="logs/trial-"$trial".log" \
          -v cpu="$cpu" -v strategy="$strategy" -v sha="$git_sha" '
          /BENCH,/ {
            sub(/^.*BENCH,/, "")
            if ($1 == "experiment" || $1 == "shadow_capacity_note")
              next
            if (NF == 8)
              print $0 "," trial "," log "," cpu "," strategy "," sha ",64,\"xv6-public-x86-NSHADOW64-QUOTA16\""
          }' >> "$raw"
      sed 's/\r$//' "$trial_log" |
        awk -F, -v trial="$trial" -v log="logs/trial-"$trial".log" \
          -v cpu="$cpu" -v strategy="$strategy" -v sha="$git_sha" '
          /SHADOW_METRICS,/ {
            sub(/^.*SHADOW_METRICS,/, "")
            if (NF == 10)
              print trial "," $0 "," log "," cpu "," strategy "," sha
          }' >> "$shadow_raw"
      if sed 's/\r$//' "$trial_log" |
         grep -E '^BENCH,.*,(ERROR)$' >/dev/null; then
        printf '%s,%s,%s,all workload rows OK,guest ERROR row,FAIL,logs/trial-%s.log\n' \
          "$trial" "$cpu" "$strategy" "$trial" >> "$trial_summary"
        echo "A guest workload reported ERROR in configuration $trial; see $trial_log" >&2
        exit 1
      fi
      printf '%s,%s,%s,XBENCH DONE,completed,PASS,logs/trial-%s.log\n' \
        "$trial" "$cpu" "$strategy" "$trial" >> "$trial_summary"
      rm -f "$trial_fs"
      repetition=$((repetition + 1))
    done
  done
done

echo "Benchmark configurations: $trial"
echo "Raw measurements: $raw"
echo "Guest logs: $logs"
"$root/tests/summarize-benchmarks.py" "$raw" "$results"
"$root/tests/plot-benchmarks.py" "$raw" "$results/graphs"
