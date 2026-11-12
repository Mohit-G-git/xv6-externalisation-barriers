# Speculation-aware console and pipe I/O

## Console writes

`sys_write()` diverts speculative writes to the console device into a bounded
buffer indexed by speculation context. Writes to the same context append under
one lock, preserving syscall order. Ordinary console writes continue through
the original device path. If the context table or its 2 KiB buffer is full,
`write()` fails; it never prints the rejected bytes early.

Outermost commit publishes console bytes only after the filesystem publisher
waits for the journal epoch to complete, refreshes the base inode view, and
discards the committed shadow state. Abort discards held console bytes. Nested
commit appends a child context's bytes to its parent; nested abort drops only
the child's bytes.

The filesystem journal and the console are separate durability domains. A
crash after the journal's durable commit point but before
`spec_output_publish()` can leave the filesystem changes committed while the
corresponding console output was never emitted. Console output cannot be
replayed after reboot, and the implementation does not claim atomic durability
across the disk and terminal.

## Pipe ownership and visibility

Each byte in the existing pipe ring carries its writer's speculation ID
(`0` means ordinary output). A reader may consume ordinary bytes or bytes
tagged with its own context. A reader from another context sleeps while a
speculative byte is at the FIFO head; it cannot skip that byte and overtake it.
This classifies inherited and duplicated descriptors by the calling process's
current context while keeping the pipe itself shared.

Speculative writers put tentative bytes in the ring immediately. Same-context
readers can therefore communicate before commit. Other readers cannot see
those bytes until commit clears their tags. Abort removes the context's
unconsumed bytes and compacts later ring contents in FIFO order. Nested commit
retags child bytes to the parent; nested abort removes only child bytes.
Publication, discard, and merge wake blocked readers and writers. A speculative
write returns a short count rather than sleeping when the ring is full, which
prevents a transaction from waiting indefinitely on an outside reader that is
not allowed to see its pending bytes. Ordinary writes retain xv6's blocking
behavior.

The xio regressions exercise same-context communication, outside-reader
blocking through commit, abort suppression, mixed readers, duplicated
descriptors, two speculative writers sharing a pipe, wraparound, capacity,
reader exit, blocked-reader lifecycle transitions, and ordinary pipes. The
QEMU harness checks that leak/failure markers are absent and that console and
pipe output appears only after the relevant commit marker.

Run `xio io-metrics` in the guest to print a 512-byte ordinary-versus-buffered
console tick comparison and ordinary-blocked versus speculative-full pipe
write ticks. The guest prints the measured byte count with those values. xv6's
`uptime()` has coarse tick resolution, so repeat runs and report the raw rows
rather than interpreting a zero-tick result as zero work.

## Limits and measurements

The console buffer is bounded to 2 KiB per context and the registry has a
fixed number of context slots. Pipe storage remains the fixed xv6 ring size;
speculative writes can be short when it fills. Pipe membership is sampled
from the process context at each syscall, so a process cannot use an ancestor's
pending bytes from a nested child context until that child merges. The current
regression suite checks correctness markers; timing and buffer-overhead
measurements can be collected with `xio io-metrics` on a QEMU environment.
