# Speculation locking and validation

The implementation uses the following lock hierarchy:

1. `spectable.lock` protects context state, process references and the
   in-flight buffer-operation count.
2. `shadow_table.lock` protects the hash chains, free list, entry lifetime
   pins and deduplicated read-set. Each allocated entry has a separate
   `data_lock` protecting its private block image.
3. `versions.lock` protects committed block-version counters.
4. xv6 buffer sleeplocks and the existing log lock retain their normal roles.

No disk I/O, `begin_op()`, `end_op()`, or buffer acquisition is performed
while `spectable.lock` is held.  `xcommit` changes ACTIVE to COMMITTING,
waits for borrowers of shadow buffers to leave, then validates and copies
private images through `log_write_real()`.  `xabort` drops the images and
read-set without publishing either.

The current implementation serializes only shadow-map metadata with one
short spinlock. A lookup pins its entry, releases that lock, and copies under
the entry's `data_lock`; reclamation checks the pin count before reuse.
Consequently independent shadow blocks can be read or updated concurrently.
The remaining scalability limitation is hash-map metadata, not private block
contents.

Version counters are volatile because shadow transactions are volatile: after
a reboot both start from an empty epoch.  First reads are recorded once per
`(transaction, device, block)` and commit rejects an entry if a committed
version has changed.  The selected policy is abort-on-conflict.
