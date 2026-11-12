# Transaction Architecture and Interface Design (xv6-x86)

## 1.3 Transaction architecture and shared APIs

### Ownership model

`struct proc` stores only an integer speculation handle (`spec_id`, zero meaning none). The registry owns lifecycle metadata and references. The shadow subsystem alone owns shadow slots, block mappings and shadow-data allocation. Filesystem/buffer-cache code asks the shadow API for read/write versions. The external-I/O layer owns held console/pipe output. The commit/recovery owner controls journal publication and reports completion to the lifecycle manager.

No subsystem may create a parallel shadow map or free another subsystem's storage. The declarations below are an integration contract proposal; implementation should land with the owning member and be reviewed before other components depend on it.

### Context model

The initial x86 registry is a fixed table of 16 entries (the capacity should be coordinated with the eventual shadow quota). A registry entry contains:

```c
enum spec_state { SPEC_FREE, SPEC_ACTIVE, SPEC_COMMITTING,
                  SPEC_COMMITTED, SPEC_ABORTED };

struct spec_ctx {
  int id;                 // generation-tagged positive handle
  int owner;              // owner PID
  enum spec_state state;
  uint proc_refs;         // participating live processes
  uint op_refs;           // admitted operations in kernel
  uint generation;        // stale-handle protection
  // Owned through subsystem APIs, not duplicated here:
  // shadow set, read/write sets, buffered console/pipe output.
};
```

`id` combines a registry slot and generation so a stale integer cannot identify a later reuse of the slot. Registry lookup validates both generation and state. Each forked process owns one process reference. Every operation that accesses context-owned storage acquires a temporary operation reference and releases it on every return path.

### Lifecycle interface

The current x86 implementation exposes these lifecycle hooks in `spec.h`; subsystem-specific shadow/output hooks remain provisional:

```c
void specinit(void);                          // boot initialization
int  spec_begin(struct proc *p);              // allocate + attach; fail if already joined
void spec_fork_attach(struct proc *parent,
                      struct proc *child);    // retain then copy ID before RUNNABLE
void spec_proc_exit(struct proc *p);          // owner abort policy + release membership
int  spec_op_enter(struct proc *p, struct spec_op *op); // ACTIVE only; pin context
int  spec_op_enter_exit(struct proc *p, struct spec_op *op); // exit cleanup ref
void spec_op_leave(struct spec_op *op);
void spec_op_abort_only(struct spec_op *op);
int  spec_file_attach(int spec_id);
void spec_file_detach(int spec_id);
enum spec_state spec_context_state(int spec_id);
int  spec_commit(struct proc *p);              // owner check, freeze, delegate publication
int  spec_abort(struct proc *p);               // owner check, freeze/discard
```

Subsystem integration points:

```c
// Shadow-storage owner (Member 2); exact implementation names are coordinated:
int  shadow_read_image(int spec_id, uint dev, uint blockno, char *dst);
int  shadow_write_image(int spec_id, uint dev, uint blockno, const char *src);
int  shadow_snapshot_begin(int spec_id, struct shadow_snapshot *snap);
int  shadow_snapshot_next(struct shadow_snapshot *snap,
                          struct shadow_block *block);
void shadow_snapshot_end(struct shadow_snapshot *snap);
int  shadow_freeze(int spec_id);
int  shadow_thaw(int spec_id);
void shadow_discard(int spec_id);
int  spec_reservation_claim(int spec_id, int kind, uint dev, uint number,
                            int state, struct spec_reservation_token *token);
int  spec_reservation_check(int spec_id, int kind, uint dev, uint number);
int  spec_reservation_state(int spec_id, int kind, uint dev, uint number);
void spec_reservation_cancel(struct spec_reservation_token *token);
void spec_reservation_finish(struct spec_reservation_token *token);
void spec_reservation_discard(int spec_id);

// External-I/O owner (Member 3):
int  spec_output_write(int spec_id, int fd_kind, void *endpoint,
                       const char *data, int len);
int  spec_output_publish(int spec_id);
void spec_output_discard(int spec_id);

// Journal/commit owner (Member 4):
int  spec_publish_blocks(int spec_id, struct spec_publish_token *token);
void spec_publish_end(struct spec_publish_token *token);
```

The shadow API must distinguish “not shadowed” from “error” (for example, return 0 for base-view fallback, 1 for a shadow hit, and -1 for failure). `shadow_write` must copy a real block into a private version on first write. Once frozen, write requests fail. `spec_publish_blocks` must document whether it can return only after `end_op()` has actually completed commit; because current `end_op()` can return while another operation remains outstanding, a stronger completion/admission mechanism may be needed.

### Locking and state transitions

- A registry spinlock protects slot allocation, generation, state, IDs and reference counts. It is never held during sleeping locks, disk I/O, `sleep()`, or callbacks.
- Transition to COMMITTING is serialized under the registry lock. New `spec_op_enter()` calls then fail. The commit path waits for `op_refs == 0` using a sleep channel and a condition loop, with no spinlock held across the sleep.
- Lock order among subsystem locks must be agreed before implementation. Initial rule: acquire a context operation reference first, release registry lock, then enter shadow/cache/inode/log locks; do not reacquire the registry lock while holding a subsystem lock.
- Final cleanup occurs after the context is terminal and both process and operation references reach zero. Cleanup delegates shadow and output release to their owners and is guarded against duplicate execution.
- `xcommit()`/`xabort()` initially require the owner process. A later shared-process policy may allow a child to request termination, but must be explicit and tested. Nested `xbegin()` is initially rejected unless a parent/child context merge design is implemented; nested behavior is an extension and cannot silently alias a second independent transaction.

### Integration sequence

1. Land the registry and process-reference lifecycle independently of shadow storage.
2. Add the syscall ABI and owner-only lifecycle handlers; fail closed on mutations until publication/discard hooks exist.
3. Connect fork/exec/exit and verify context lifetime.
4. Integrate the agreed shadow interface with buffer cache and filesystem.
5. Integrate console/pipe output handling.
6. Connect validated shadow publication to a journal completion point and only then publish held output.

This contract is superseded by the final integration clarifications below for reservations, publication admission, and journal capacity.

## Final integration clarifications

### Operation tokens and operation coverage

`struct spec_op` contains the pinned context pointer, explicit `spec_id`, and an active bit. `spec_op_enter(p, &op)` succeeds only for an ACTIVE context and pins it before any context-owned state is touched. Callers pass `op.spec_id` explicitly through filesystem, descriptor, shadow, reservation, and output operations; kernel code must not infer the identity later from a possibly changed `proc` field. Every successful enter has exactly one leave. A failed private operation calls `spec_op_abort_only(&op)` after releasing inode, buffer, reservation, and publication-gate locks; this prevents later mutation/publication while permitting cleanup and explicit abort. Leave occurs after all subsystem locks are released.

The filesystem admission gate covers all operations that can observe or modify filesystem state: `namei`/`nameiparent` path traversal and inode reads; `open`, `create`, `unlink`, `link`, `mkdir`, `mknod`, and `chdir`; exec path resolution and executable inode reads; `readi`/`writei` and inode metadata updates; `fstat`/stat; ordinary non-speculative writes and their `begin_op`/`log_write`/`end_op` transaction; `fileclose`/`iput` paths that may truncate; and speculative operations reading their base view. Admission is acquired before path/inode/cache access and released only after the complete logical operation. Publication closes admission, drains admitted operations, publishes, refreshes affected inode-cache entries under the agreed lock order, and only then reopens admission. Thus no filesystem observer can use a stale cache image across publication. Console and pipe handling follows Stage 1's separate visibility policy.

### Reservation state transitions

Member 2 owns global arbitration for speculative and ordinary inode/block allocations. A reservation key includes resource kind, device, and resource number. Each speculative context has one final desired state per key: `ALLOCATED` or `DEFERRED_FREE`; a deferred free reserves the base resource from reuse until commit/abort resolution. An ordinary allocation obtains the same global claim while selecting/updating the base bitmap or inode table and releases its temporary claim only after the base allocation is recorded.

Reservation mutation returns an operation-scoped token containing the previous state (including “no reservation”), previous revision, installed state, and installed revision. Cancellation restores the previous state only if the reservation still has that token's installed revision. This prevents a failed overlapping operation from removing a later successful claim. On successful filesystem operation, the context's final state remains. Repeated allocation while already `ALLOCATED` and repeated free while already `DEFERRED_FREE` are idempotent. Allocate then free changes `ALLOCATED -> DEFERRED_FREE`, so the committed result is free; free then allocate changes `DEFERRED_FREE -> ALLOCATED`, so the committed result is allocated. Abort releases all claims and preserves base state. Commit applies the final state once, then releases claims after durable publication. Releasing a resource cannot make it available to ordinary allocators before the commit point.

### Frozen write set and journal capacity

Member 2 freezes the shadow context after operation references drain and returns a stable snapshot of unique `(device, block number, full block image)` entries. Snapshot lifetime pins its storage until explicit end/discard. No writes or reservation changes are admitted while frozen. Member 4 validates the *complete* snapshot before publication against the actual usable capacity of the current xv6 log transaction, derived from on-disk `log.size` and current log-header occupancy, with the log header block and any required deduplication accounted for. It must not rely on the compile-time `MAXOPBLOCKS` alone. If the unique write set cannot fit in one transaction, publication fails before touching any home block. Member 4 publishes the complete set in one journal transaction and reports success only after the durable commit point and cache installation are complete. There is no success result for multi-transaction partial publication.

The publication token closes and owns the filesystem admission gate from preflight through durable journal completion and inode-cache refresh. Ordinary filesystem operations, including ordinary writes that call `begin_op`, must enter this same gate before starting their journal operation; they cannot bypass it. If preflight or publication fails before any home block is installed, the token may be released and lifecycle may return the context to ACTIVE (or abort-only if its private operation failed). Once any home block may have been installed, the journal owner must complete/recover the transaction or report a terminal outcome; lifecycle must not reopen the context as ACTIVE. Member 4 owns the journal internals and the publication gate implementation; Member 3 owns lifecycle orchestration and refreshes inode overlays/cache through the agreed callback while the token remains held.

### Inode overlays, cleanup, and lock order

Member 3 owns context-specific inode metadata overlays and namespace decisions. A speculative inode lookup first consults that context's overlay and then reads its context-aware block view; an outside lookup reads only the base view. The shared inode cache must not be modified with speculative metadata. Create/unlink updates the private inode/directory/bitmap block images and overlay, while allocation claims prevent collisions; frees are deferred. Create-then-unlink and unlink-then-create update the same private final state and reservation tokens as above. On partial create/unlink failure, cancel only that operation's claims and overlay edits; if the private block view cannot be restored consistently, mark the context abort-only and leave base state untouched.

Lock ordering: acquire a speculation operation reference first, then the filesystem admission token, then inode sleeplocks, then buffer-cache sleeplocks, then the journal's internal serialization as required. Reservation-table spinlocks are held only for short claim/state transitions and are released before inode/buffer I/O or sleep. Registry spinlocks are never reacquired while holding filesystem, reservation, shadow, or journal locks. Publication takes the exclusive admission token, drains readers/writers, freezes the shadow snapshot, validates capacity, invokes the journal, refreshes inode cache while admission remains closed, and releases in reverse order. No spinlock is held across sleep, disk I/O, or callbacks. Member 2 and Member 4 must document any internal lock-order exception before integration.

### Ownership and failure boundary

Member 2 implements shadow image access, stable snapshots, global inode/block reservation claim/check/cancel/finish, and shadow cleanup in its subsystem files. Member 3 implements operation-token lifecycle/orchestration, inode overlays, filesystem call-site integration with the admission API, create/unlink, descriptor reads/stat in `file.c`, and post-publication cache refresh. Member 4 implements the admission/publication token, actual log-capacity preflight, and one-transaction atomic journal publication in `log.c`. Member 3 may add declarations and call sites at agreed boundaries but must not replace Member 2 shadow/reservation internals or Member 4 journal internals. If any required API is absent, the Member 3 caller remains fail-closed and records that dependency; it must not fall back to ordinary `bread()`/`log_write()` for speculative mutations.

## Shadow storage

`xv6-public/shadow.c` provides the bounded private-block store. `NSHADOW`
entries hold a speculation ID, device,
block number, allocation state, reference count, and one `BSIZE` data image.
Entries are indexed by a chained hash table using
`(speculation_id, device, block_number)`, so the same real block can have
independent versions in different speculations and duplicate entries cannot be
created for one speculation.

The shadow table uses independent bucket spinlocks for hash chains and entry
pins, per-entry data locks for private images, and a metadata spinlock for the
free list, global counters, freeze state, and version/read-set metadata.
`shadow_write_image` installs a complete private image
supplied by the caller (the filesystem helper merges the base image before
its first partial update), enforces the per-speculation `NSHADOW_QUOTA` and
global `NSHADOW` limits, and returns `-1` on exhaustion. It never writes
through to the real block. `shadow_read_image` returns `1` for a private hit,
`0` for base-view fallback, and `-1` for invalid arguments. Freeze prevents
new writes; snapshot begin/next/end enumerates unique full-block images from
the stable frozen set. `shadow_discard` is idempotent and releases both shadow
slots and context reservations during terminal cleanup.

The reservation table arbitrates ordinary and speculative inode/block
allocation. Per-operation tokens carry state and revision numbers so
cancellation restores prior state without erasing a later successful claim.

Each entry is 512 bytes of data plus 24 bytes of metadata on x86 (32-bit
alignment), for 34,304 bytes of entry storage across the 64-entry table,
before hash-head, free-list, lock, and allocator overhead. The table tracks
current occupancy and peak occupancy through `shadow_count`,
`shadow_capacity`, and `shadow_peak`.

The capacity milestone is validated by allocating distinct keys until the
per-speculation quota is reached, confirming that the next allocation returns
`-1`, and then discarding the context and confirming that its slots can be
reused. Repeating this with multiple speculation IDs verifies that identical
device/block pairs remain independent and that discard does not leak entries.

## Current implementation boundary

The x86 lifecycle registry and syscall ABI are implemented. The syscall
dispatcher pins an operation across non-lifecycle syscalls; exit-time
descriptor/cwd cleanup has a separate reference path. Fork inherits
membership, exec preserves it, owner exit aborts, and context-bound inode file
descriptors retain terminal context state until close. Stage 1 console and
pipe isolation remains implemented.

Filesystem block images route through `shadow_read_image` and
`shadow_write_image` in `fs.c`. Speculative updates do not call `log_write`
or modify base buffer-cache images. Inode-cache keys include the speculation
ID, providing separate metadata views. Path traversal, exec, descriptor
reads/stat/writes, create, link, unlink, mkdir, mknod, chdir, and close/truncate
paths use the context view. Global reservations arbitrate inode and block
allocation and represent deferred frees. Partial private-storage failures
mark the context abort-only.

The journal publisher and exclusive filesystem publication gate are present in
the x86 implementation. `xcommit()` freezes the context, validates the private
set and recorded base versions, reserves the complete write set against the
current usable journal capacity (`min(LOGSIZE, log.size - 1)` minus current
header occupancy), installs the stable snapshot through real buffers, waits for
journal completion, refreshes base inode views, and only then publishes held
output. A conflicting base block or a set larger than the actual journal
capacity fails before home-buffer publication and leaves the context abortable.
Abort invalidates speculative inode views and discards shadow blocks,
reservations and held output. Read and write base-version validation is implemented with a deduplicated read
set and conservative abort-on-conflict. This is MVCC-inspired optimistic
validation; durable multi-version snapshots remain outside the scope.

Nested `xbegin()` is currently rejected. Aborted descendants retain the aborted association and cannot mutate until they explicitly begin a fresh context. Committed descendants detach when they next attempt a mutation. These rules avoid treating an aborted shared handle as an ordinary writable process.
