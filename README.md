# xv6-externalisation-barriers

OS research project implementing speculative-process externalization barriers
for xv6-public x86. The current implementation covers lifecycle management,
private filesystem block images, namespace isolation, bounded journal-backed
commit, abort cleanup, write/read base-version checks, per-bucket shadow
synchronization, buffered console/pipe output, and nested speculative scopes.
Nested scopes merge into their parent in stack order; only the outermost commit
publishes changes. Durable multi-version snapshots are not implemented.

Build with `make clean && make` from `xv6-public/`. QEMU is auto-detected when
available, or can be selected explicitly with
`make QEMU=/absolute/path/to/qemu-system-i386 qemu-nox`.

Guest checks include `xspec`, `xnested`, the relevant `xio` modes, and stock
`usertests`.
See `TEST_RESULTS.md` for exact commands and runtime status.
