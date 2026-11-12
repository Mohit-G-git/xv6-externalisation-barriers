# Speculation Threat Model and Isolation Invariants (xv6-x86)

## Protected effects

The kernel must prevent a speculative process or descendant from exposing changes through:

1. Data blocks written through `writei()` and buffer-cache operations.
2. File creation, deletion, links, inode link counts and directory contents.
3. Allocation bitmap changes and newly allocated or freed blocks.
4. User output to the console device.
5. Pipe bytes consumed by a process outside the same speculation.
6. Shared inode and buffer-cache state that could make a private update visible before commit.

Kernel diagnostics (`cprintf`/`panic`) are kernel-generated, not user writes; they remain outside the user transaction unless a later debugging policy explicitly tags them. Reads may affect shared caches but must not change the logical view returned to an outside process.

## Participants and visibility

- A process calling `xbegin()` joins a new speculation context. Descendants created by `fork()` inherit that same context.
- Members see committed base state plus all updates made by the shared context (read-your-writes). Parent and child updates are part of one shared write set.
- Unrelated processes read the committed base state and do not see speculative directory, inode or file-data changes.
- Console output and pipe data to outside readers remain buffered until successful publication. A pipe write to a reader inside the same context remains immediately usable, so pipelines do not deadlock.
- `exec()` changes the process image but does not leave its context.
- On abort, private blocks and held output are discarded. On commit, all supported filesystem effects are published as one logical transaction before held external output is delivered.

## Isolation invariants

1. **No early disk visibility:** While ACTIVE or COMMITTING, only members may resolve the context's private versions; outside readers use the real view.
2. **No early output:** A console/pipe effect destined outside the context is not delivered before successful filesystem publication.
3. **Read-your-writes:** Every member read resolves a block from the shared speculative version when one exists, else the committed base version.
4. **No write-through fallback:** Shadow allocation/capacity failure rejects the write. It never falls through to mutation of the real block.
5. **Frozen write set:** Transition to COMMITTING closes admission to new speculative operations and waits until already admitted operations leave before validation/publication.
6. **One terminal outcome:** ACTIVE may become COMMITTING then COMMITTED, or ACTIVE may become ABORTED. Terminal states are immutable.
7. **Lifetime:** A context cannot be reclaimed while it has process references, active-operation references, or subsystem cleanup/publication in progress.
8. **Fork atomicity:** Child membership and its reference are established before the child becomes RUNNABLE; failed fork releases any acquired reference.
9. **Exit safety:** Every exiting participant drops exactly one membership reference. Owner exit triggers abort. Context cleanup is idempotent.
10. **Conflict rule:** The implementation uses conservative abort-on-conflict. Version checks cover each written block and each first-read block before publication; versions are volatile and reset after reboot.
11. **Locking:** Registry and state locks protect metadata only. Blocking disk operations, sleep, and publication callbacks do not run while holding registry spinlocks; xv6's `sleep(chan, lk)` atomically releases and reacquires its supplied lock.
12. **Supported atomicity limit:** A commit either publishes its entire supported disk write set through one journal transaction or fails before publishing any block. Multi-transaction partial publication is not called atomic.

## Threat assumptions and limits

The kernel is trusted; user processes can issue arbitrary valid syscalls and can fork/exit at adversarial times. DMA devices, power-loss guarantees beyond the xv6 IDE/log model, and kernel memory corruption are outside this project's threat model. The existing journal gives filesystem crash recovery, but console and pipe delivery cannot be made crash-atomic with disk state. The implementation must state the journal commit point and external-output point separately.

## Verification tools and limits

The repository includes host automation for guest functional tests, stock xv6
`usertests`, and controlled QEMU termination around the redo-journal protocol.
Crash recovery guarantees are evaluated against the filesystem image after
restart: an absent durable header means rollback, and a durable header means
replay. A crash after durable disk commit but before console publication may
leave the committed filesystem state without its console output. This is an
expected limitation, not a transaction failure.

The included benchmark reports measured QEMU/xv6 tick counts and does not
establish a security guarantee, serializability, or parallel speedup. Runtime
race tests are bounded empirical checks; they cannot prove the absence of all
concurrency bugs.
