# xv6-x86 Architecture and Speculation Integration Notes

## Scope and compatibility

The workspace contains the MIT `xv6-public` x86 tree, not xv6-RISC-V. The project is being adapted to the x86 implementation as requested. This document records the existing x86 paths before kernel changes. The source tree is `xv6-public/`; kernel and user sources share one directory, system-call stubs are generated from `usys.S`, and system calls enter through `int $T_SYSCALL` with the call number in `eax` and arguments on the user stack.

## Boot and execution flow

`main()` in `main.c` initializes memory, CPU/APIC state, console, process table, traps, buffer cache, file table, and IDE, then starts application processors and creates the first user process. Each CPU enters `scheduler()` in `proc.c`, selects a runnable process while holding `ptable.lock`, switches address spaces, and context-switches with `swtch.S`. A user trap enters via `trapasm.S`; `trap()` dispatches `T_SYSCALL` to `syscall()` and returns through the saved trap frame. Timer interrupts update `ticks`, wake sleepers, and may yield the running process.

## System calls and user boundary

`user.h` declares the user API. `usys.S` emits syscall stubs that load the syscall number into `eax`, execute `int $T_SYSCALL`, and return the result. `syscall.h` assigns syscall numbers; `syscall.c` indexes a function-pointer table and fetches integer/pointer/string arguments from the user stack and address space. Implementations live in `sysproc.c` (process/time calls) or `sysfile.c` (file calls). New `xbegin`, `xcommit`, and `xabort` calls therefore require edits to `user.h`, `usys.S`, `syscall.h`, `syscall.c`, and a kernel implementation, plus user programs and the `UPROGS` list in the root Makefile.

## File and storage paths

The ordinary file write path is `sys_write()` → `filewrite()` → `writei()` → `bmap()` / `balloc()` → `bread()` → modify block data → `log_write()` → `end_op()`. `filewrite()` deliberately divides large writes into multiple filesystem operations to stay within `MAXOPBLOCKS`; this means one user `write()` can span multiple log commits. `sys_open()` and `sys_mkdir()` use the internal `create()` helper, and `sys_unlink()` updates directory entries, inode link counts, inode blocks and allocation state. Directory lookup and path traversal use `namei()`, `nameiparent()`, `dirlookup()` and `readi()`.

`fs.c` holds the inode cache and filesystem primitives: `ialloc()`, `iupdate()`, `balloc()`, `bfree()`, `bmap()`, `readi()`, `writei()`, directory operations and path traversal. The superblock lays out the log, inode table, allocation bitmap and data area. Since inode metadata, directory entries, bitmap blocks and file data all travel through the block layer, redirecting block reads and writes can cover create/unlink as well as file contents.

`bio.c` implements the shared buffer cache. `bread()` gets a cached block and reads it through IDE on a miss; each buffer has a sleeplock. `bwrite()` writes the locked buffer to disk, while `brelse()` releases it. The global cache lock protects buffer lookup/reuse and reference counts; a per-buffer sleeplock serializes access to its contents. A speculative block layer must preserve the real cached block for outside readers and route speculative readers/writers to transaction-owned versions. Cache buffers cannot themselves be treated as transaction ownership because they are shared globally and evictable.

## Logging and publication

`sysfile.c`, `exec.c`, `file.c` and other filesystem clients bracket modifications with `begin_op()` and `end_op()`. `log_write()` records unique block numbers, pins buffers dirty, and absorbs repeated writes to the same block. When the final outstanding filesystem operation ends, `end_op()` sets `committing`, releases the log spinlock, and calls `commit()`. `commit()` copies modified buffers to the on-disk redo log, writes the header (the durable commit point), installs blocks at home locations, and clears the header. It then clears `committing` and wakes waiters.

Consequently, an individual `end_op()` may return without committing if other operations remain outstanding; even on the last operation, it returns only after the commit finishes. Speculation publication must coordinate with the actual completion of its own outer transaction, not infer durability from a non-final `end_op()`. Existing log capacity is `LOGSIZE = MAXOPBLOCKS*3` (30 blocks here), each ordinary filesystem operation reserves up to 10 blocks, and `filewrite()` chunks large writes. The project must choose and document how the final speculative write set is constrained or committed atomically.

## Process lifecycle and synchronization

`allocproc()` creates an embryo. `fork()` copies the address space, trap frame, open-file references and cwd, then marks the child runnable. A speculation association must be inherited before the child becomes runnable and must hold a context reference. `exec()` replaces the user image but does not reset unrelated process metadata, so it should retain an active speculation association. `exit()` closes files and cwd, reparents children, and becomes a zombie; `wait()` eventually reclaims the process slot. Therefore membership references should be released at exit, not delayed until `wait()`, and owner exit needs a defined abort/ownership-transfer policy to avoid an orphaned active context.

`spinlock.c` implements spinlocks by disabling local interrupts and using atomic `xchg`; they must protect short, non-blocking critical sections. `sleeplock.c` implements sleepable locks by sleeping while holding an internal spinlock only around lock state. `sleep(chan, lk)` atomically transfers protection to `ptable.lock`, marks the process sleeping, and releases/reacquires the supplied lock across scheduling. Never sleep or perform disk I/O while holding a spinlock. The relevant lock order and context lifetime rules must be documented before adding a registry.

## Externalization paths and initial invariants

- **Disk state:** file writes, creates, unlinks, directory changes, inode updates and allocation changes become block modifications via `fs.c` and the buffer/log path.
- **Console:** device writes flow through `filewrite()` → `writei()` → device switch `devsw[CONSOLE].write` → `consolewrite()` in `console.c`; kernel diagnostics via `cprintf()` are a separate path and should not be treated as user speculative output without an explicit policy.
- **Pipes:** `filewrite()` routes pipe descriptors to `pipewrite()` in `pipe.c`, which writes bytes into a shared ring buffer and wakes readers. Buffering every pipe write would deadlock same-speculation pipelines, so publication policy needs to distinguish reader membership from outside readers.
- **Shared state:** inode cache, buffer cache and open file objects are shared kernel objects. Any speculative view must be selected by the calling process/context rather than mutating a shared real cache entry in place.

Initial isolation requirements: speculative participants and descendants read their own writes; outside processes observe the real filesystem and receive no speculative output; abort discards private versions and buffered external output; commit validates and publishes the full supported write set before releasing external output; no operation may mutate a context after it is frozen; a context remains alive while any participant or in-flight kernel operation references it. Conflicts with outside writes require one explicit policy (abort-on-conflict is the conservative initial choice) and version tracking if validation is to detect stale reads or writes.

## Proposed source dependency map and modification points

| Concern | Existing path | Likely x86 integration points |
|---|---|---|
| System call ABI | `user.h` → `usys.S` → `syscall.h` / `syscall.c` → `sysproc.c` | Add the three APIs and dispatch entries; use the x86 stack argument helpers. |
| Context registry | `proc.c`, `proc.h`, `param.h`, `defs.h` | Add a separate registry/context module and declarations; add only a context ID/reference field to `struct proc`. |
| Fork/exec/exit | `fork()`, `exec()`, `exit()`, `wait()` | Inherit and retain association on fork; preserve on exec; release on exit; define owner-exit cleanup. |
| Private block view | `bread()` / `bwrite()` / `log_write()` and `struct buf` | Coordinate one shadow-storage API with the storage owner; avoid a second ownership table in process management. |
| Filesystem metadata | `writei()`, `bmap()`, `balloc()`, `bfree()`, `iupdate()`, `dirlink()`, `sys_unlink()` | Prefer block-level redirection to cover file data and metadata consistently. |
| Console / pipes | `consolewrite()`, `pipewrite()`, file descriptor dispatch in `filewrite()` | Coordinate buffering/publication and same-context pipe semantics with the external-I/O owner. |
| Commit / abort | `begin_op()`, `end_op()`, `log_write()`, `commit()` | Lifecycle handlers call agreed freeze/publish/discard interfaces; journal integration belongs to commit/recovery owner. |
| Debug events | context transitions and subsystem interfaces | Add optional structured event hooks only after event format and lock-safe logging behavior are agreed. |

## x86 adaptation decisions

1. Preserve the assignment’s behavior and member boundaries, but translate platform details: x86 syscall entry is `int $T_SYSCALL`, the user stubs are in `usys.S`, filesystem sources are flat under `xv6-public/`, and QEMU is `qemu-system-i386`.
2. Use xv6-x86's actual `sleep(chan, spinlock*)` interface and its ordinary `LOGSIZE` / `MAXOPBLOCKS` constants; do not copy RISC-V-only helpers such as `sleep_prepare`, `copyin`, `copyout` signatures or `usys.pl` edits.
3. Keep the transaction registry separate from `struct proc`; process membership is an ID/reference, not the owner of shadow blocks. Shadow storage, buffer-cache routing, external I/O, and durable commit remain coordinated with their assigned owners.
4. Implement in reviewable stages. This document completes the architecture-analysis deliverable only; it does not claim that speculation or transactional isolation has been implemented.
