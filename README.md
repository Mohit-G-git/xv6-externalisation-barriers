# xv6-externalisation-barriers

OS research project implementing speculative-process externalization barriers
for xv6-public x86. The current implementation covers lifecycle management,
private filesystem block images, namespace isolation, bounded journal-backed
commit, abort cleanup, write-write base-version checks, and buffered console/
pipe output. Nested speculation and full read-set MVCC are not implemented.

Build with `make clean && make` from `xv6-public/`. QEMU is auto-detected when
available, or can be selected explicitly with
`make QEMU=/absolute/path/to/qemu-system-i386 qemu-nox`.

Guest checks include `xspec`, the relevant `xio` modes, and stock `usertests`.
See `TEST_RESULTS.md` for exact commands and runtime status.
