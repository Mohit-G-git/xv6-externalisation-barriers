#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
qemu=${1:-${QEMU:-}}
status=0

for suite in run-tests.sh crash-recovery.sh run-benchmarks.sh; do
  if [ -n "$qemu" ]; then
    "$root/tests/$suite" "$qemu" || status=1
  else
    "$root/tests/$suite" || status=1
  fi
done

exit "$status"
