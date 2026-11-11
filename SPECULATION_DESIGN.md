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
int  spec_op_enter(struct proc *p,
                   struct spec_ctx **out);    // ACTIVE only; pin operation
void spec_op_leave(struct spec_ctx *ctx);
int  spec_commit(struct proc *p);              // owner check, freeze, delegate publication
int  spec_abort(struct proc *p);               // owner check, freeze/discard
```

Subsystem integration points:

```c
// Shadow-storage owner (Member 2):
int  shadow_read(int spec_id, uint dev, uint blockno, char *dst);
int  shadow_write(int spec_id, uint dev, uint blockno, const char *src);
int  shadow_validate(int spec_id);             // conflict check
void shadow_discard(int spec_id);

// External-I/O owner (Member 3):
int  spec_output_write(int spec_id, int fd_kind, void *endpoint,
                       const char *data, int len);
int  spec_output_publish(int spec_id);
void spec_output_discard(int spec_id);

// Journal/commit owner (Member 4):
int  spec_publish_blocks(int spec_id);          // success only after journal completion
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

This contract is an initial member interface proposal; cross-member agreement is still needed for registry capacity, shadow return codes, conflict/version semantics, pipe endpoint membership, and journal completion notification.

## Current implementation boundary

The x86 lifecycle calls and generation-tagged registry are implemented in `syscall.c`, `sysproc.c`, `user.h`, `usys.S`, `proc.c` and the Makefile. Fork inherits a process reference; exec naturally preserves the association; process exit drops its reference and owner exit aborts the context. Commit and abort freeze operation admission and wait for operation references. The filesystem has no shadow, external-output, or journal-publication integration yet, so speculative `write`, create, link, unlink, mkdir and mknod calls fail closed. `xcommit()` therefore succeeds only for an empty context; `xabort()` has no shadow/output resources to discard yet. This is lifecycle infrastructure, not full speculative filesystem behavior.

Nested `xbegin()` is currently rejected. Aborted descendants retain the aborted association and cannot mutate until they explicitly begin a fresh context. Committed descendants detach when they next attempt a mutation. These rules avoid treating an aborted shared handle as an ordinary writable process.

