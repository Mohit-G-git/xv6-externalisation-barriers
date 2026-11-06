# Repair verification

All commands below were run from `xv6-public/` on the repair branch. Build
uses `-Werror`.

| Command | Result |
|---|---|
| `make clean && make -j2` | PASS |
| `make CPUS=1 QEMU=/home/kali/Desktop/qemu/build/qemu-system-i386 qemu-nox`, then `xspec` | PASS: `XSPEC_PASS` |
| same QEMU session, `xio pipe-commit` | PASS: outside reader received committed byte after commit marker |
| same QEMU session, `xio console-commit` | PASS: observer/payload/done ordering observed |
| stock `usertests`, CPUS=2 | Incomplete: run reached the long `bigdir` phase before the session was stopped |
| crash-recovery matrix | Not run; no crash harness is claimed |

The explicit QEMU path is the desktop binary. The Makefile now supports both
PATH discovery and an explicit `QEMU=` override. Historical logs under
`results/` are not presented as evidence for this repair revision.
