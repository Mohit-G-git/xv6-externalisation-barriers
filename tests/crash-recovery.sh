#!/bin/sh
set -eu

qemu=${1:-qemu-system-i386}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
kernel_dir="$root/xv6-public"
work=$(mktemp -d "${TMPDIR:-/tmp}/xv6-crash-test.XXXXXX")

cleanup()
{
  rm -f "$work/fs.img"
  rmdir "$work"
}
trap cleanup EXIT HUP INT TERM

cd "$kernel_dir"
make clean
make -j2 fs.img xv6.img

for point in 1 2 3; do
  echo "Testing journal crash point $point"
  make clean
  make -j2 SPEC_CRASH_POINT="$point" fs.img xv6.img
  cp fs.img "$work/fs.img"

  expect "$root/tests/crash-point.exp" "$qemu" xv6.img "$work/fs.img" \
    "$point"

  make clean
  make -j2 fs.img xv6.img
  if [ "$point" -eq 1 ]; then
    expected=verify-absent
    marker="XCRASH RECOVERY ABSENT PASS"
  else
    expected=verify-present
    marker="XCRASH RECOVERY PRESENT PASS"
  fi
  expect "$root/tests/recovery-check.exp" "$qemu" xv6.img "$work/fs.img" \
    "$expected" "$marker"
done

echo "Crash recovery passed at all three journal points."
