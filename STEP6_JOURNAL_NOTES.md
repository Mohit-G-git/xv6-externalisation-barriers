# Step 6 journal integration notes

The x86 implementation supports atomic speculative publication for a final
shadow set of at most `MAXOPBLOCKS` distinct blocks (10 with the current xv6
configuration). Larger sets are rejected before any real buffer is modified;
the caller can abort and reclaim the private set.

Commit publication closes the filesystem publication gate before conflict
preflight and copies frozen shadow images into ordinary buffers. It calls
`log_write()` for every distinct block. `end_op()` may only finish the caller's
filesystem operation while another operation remains outstanding, so the
publisher records the epoch only after journal admission and waits for that
admitted operation's commit epoch to advance. Buffered
console and pipe output is released only after that completion point.

Each first speculative block image records the committed base generation. The
exclusive publication phase compares all recorded generations before touching
home buffers and rejects a stale write set. Full read-set validation remains a
Step 8 extension.

Abort never enters the real buffer cache. It discards shadow blocks,
reservations, inode views and held external output. Repeated abort attempts
return an error after the context has been reclaimed.

The journal header write is xv6's durable commit point. Installing home blocks
and releasing external output happen afterward; therefore filesystem recovery
atomicity is stronger than the external-output delivery guarantee.
