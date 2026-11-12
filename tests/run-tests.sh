#!/bin/sh
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel_dir="$root/xv6-public"
results="$root/results/tests"
logs="$root/results/logs"
qemu=${1:-${QEMU:-}}
test_filter=${TEST_FILTER:-}

if [ -z "$qemu" ]; then
  qemu=$(command -v qemu-system-i386 || command -v qemu-system-x86_64 || true)
fi

mkdir -p "$results" "$logs"
summary="$results/test-summary.csv"
report="$results/test-summary.txt"
log="$logs/functional-tests.log"
build_log="$logs/functional-build.log"

tests='
xspec|xspec|XSPEC PASS (filesystem isolation and lifecycle)
xnested|xnested|XNESTED PASS (nested merge and abort)
xexec|xexec|EXEC_COMMIT_DONE
xio-console-abort|xio console-abort|CONSOLE_ABORT_DONE
xio-console-commit|xio console-commit|CONSOLE_COMMIT_DONE
xio-console-capacity|xio console-cap|CONSOLE_CAPACITY_OK
xio-pipe-same|xio pipe-same|PIPE_SAME_DONE
xio-pipe-commit|xio pipe-commit|PIPE_COMMIT_DONE
xio-pipe-abort|xio pipe-abort|PIPE_ABORT_DONE
xio-pipe-wrap|xio pipe-wrap|PIPE_WRAP_OK
xio-pipe-full|xio pipe-full|PIPE_FULL_DONE
xio-pipe-full-abort|xio pipe-full-abort|PIPE_FULL_ABORT_DONE
xio-pipe-mixed|xio pipe-mixed|PIPE_MIXED_DONE
xio-pipe-multi-writer|xio pipe-multi-writer|PIPE_MULTI_WRITER_DONE
xio-pipe-reader-exit|xio pipe-reader-exit|PIPE_READER_EXIT_DONE
xio-pipe-ordinary|xio pipe-ordinary|PIPE_ORDINARY_OK
xio-pipe-blocked-commit|xio pipe-blocked-commit|PIPE_BLOCKED_COMMIT_DRAINED
xio-pipe-blocked-abort|xio pipe-blocked-abort|PIPE_BLOCKED_ABORT_DRAINED
xio-owner-exit|xio owner-exit|OWNER_EXIT_OBSERVER
xio-child-abort|xio child-abort|CHILD_ABORT_DONE
usertests|usertests|ALL TESTS PASSED
'

printf 'test_id,cpu_count,test,expected_marker,actual_result,status,log\n' > "$summary"
missing=""
for tool in make gcc expect; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    missing="$missing $tool"
  fi
done
if [ -z "$qemu" ] || { [ ! -x "$qemu" ] && ! command -v "$qemu" >/dev/null 2>&1; }; then
  missing="$missing QEMU"
fi
if [ -n "$missing" ]; then
  echo "Required executable(s) unavailable:$missing" >&2
  total=0
  blocked=0
  while IFS='|' read -r id command marker; do
    [ -n "$id" ] || continue
    printf '%s,,"%s","%s","required dependency missing",BLOCKED,\n' \
      "$id" "$command" "$marker" >> "$summary"
    total=$((total + 1))
    blocked=$((blocked + 1))
  done <<EOF
$tests
EOF
  {
    echo "Functional xv6 test run"
    echo "Status: BLOCKED (missing dependency:$missing)"
    echo "Total: $total"
    echo "Blocked: $blocked"
    echo "Machine-readable results: results/tests/test-summary.csv"
  } > "$report"
  exit 2
fi

if ! (cd "$kernel_dir" && make clean >"$build_log" 2>&1 &&
      make -j2 QEMU="$qemu" fs.img xv6.img >>"$build_log" 2>&1); then
  cat "$build_log" >&2
  total=0
  blocked=0
  while IFS='|' read -r id command marker; do
    [ -n "$id" ] || continue
    printf '%s,,"%s","%s","kernel build failed",BLOCKED,%s\n' \
      "$id" "$command" "$marker" "logs/functional-build.log" >> "$summary"
    total=$((total + 1))
    blocked=$((blocked + 1))
  done <<EOF
$tests
EOF
  {
    echo "Functional xv6 test run"
    echo "Status: BLOCKED (clean build failed)"
    echo "Total: $total"
    echo "Blocked: $blocked"
    echo "Build command: make clean && make -j2 QEMU=<qemu> fs.img xv6.img"
    echo "Build log: results/logs/functional-build.log"
  } > "$report"
  exit 1
fi

passed=0
failed=0
not_tested=0
test_rc=0
for cpu in ${CPU_COUNTS:-"1 3"}; do
  log="$logs/functional-tests-cpu$cpu.log"
  cpu_rc=0
  if [ -n "$test_filter" ]; then
    if ! XV6_TEST_FILTER="$test_filter" XV6_CPUS="$cpu" \
         expect "$root/tests/qemu-tests.exp" "$qemu" \
         "$kernel_dir/xv6.img" "$kernel_dir/fs.img" >"$log" 2>&1; then
      cpu_rc=1
    fi
  elif ! XV6_CPUS="$cpu" expect "$root/tests/qemu-tests.exp" "$qemu" \
       "$kernel_dir/xv6.img" "$kernel_dir/fs.img" >"$log" 2>&1; then
    cpu_rc=1
  fi
  [ "$cpu_rc" -eq 0 ] || test_rc=1
  failure_seen=0
  while IFS='|' read -r id command marker; do
    [ -n "$id" ] || continue
    if [ -n "$test_filter" ] && [ "$id" != "$test_filter" ]; then
      continue
    fi
    if grep -Fqx "PASS $command" "$log"; then
      status=PASS
      actual=$marker
      passed=$((passed + 1))
    elif [ "$cpu_rc" -ne 0 ] && [ "$failure_seen" -eq 0 ]; then
      status=FAIL
      actual="failed before expected marker"
      failure_seen=1
      failed=$((failed + 1))
    else
      status=NOT_TESTED
      actual="not reached"
      not_tested=$((not_tested + 1))
    fi
    printf '%s,%s,"%s","%s","%s",%s,%s\n' "$id" "$cpu" "$command" \
      "$marker" "$actual" "$status" "logs/functional-tests-cpu$cpu.log" \
      >> "$summary"
  done <<EOF
$tests
EOF
  if [ "$cpu_rc" -ne 0 ] && [ "$failure_seen" -eq 0 ]; then
    printf 'qemu-shutdown,%s,"QEMU clean shutdown","exit status 0","unexpected QEMU/runner exit",FAIL,%s\n' \
      "$cpu" "logs/functional-tests-cpu$cpu.log" >> "$summary"
    failed=$((failed + 1))
  fi
done

qemu_version=$("$qemu" --version 2>&1 | sed -n '1p')
compiler_version=$(gcc --version 2>&1 | sed -n '1p')
total=$((passed + failed + not_tested))
{
  echo "Functional xv6 test run"
  echo "Environment: $(uname -srm)"
  echo "QEMU: $qemu_version"
  echo "Compiler: $compiler_version"
  echo "Build command: make clean && make -j2 QEMU=<qemu> fs.img xv6.img"
  echo "CPU counts: ${CPU_COUNTS:-1 3}"
  echo "Test command: expect tests/qemu-tests.exp <qemu> xv6.img fs.img (CPUS=1,3)"
  echo "Total: $total"
  echo "Passed: $passed"
  echo "Failed: $failed"
  echo "Not tested: $not_tested"
  echo "Raw logs: results/logs/functional-tests-cpu*.log"
  echo "Machine-readable results: results/tests/test-summary.csv"
} > "$report"

cat "$report"
[ "$test_rc" -eq 0 ] && [ "$failed" -eq 0 ]
