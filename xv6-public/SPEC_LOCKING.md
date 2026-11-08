# Speculation locking and validation

The implementation uses the following lock hierarchy:

1. `spectable.lock` protects context state, process references and the
   in-flight buffer-operation count.
2. `shadow_table.lock` protects the hash chains, free list, private images
   and deduplicated read-set.
3. `versions.lock` protects committed block-version counters.
4. xv6 buffer sleeplocks and the existing log lock retain their normal roles.

No disk I/O, `begin_op()`, `end_op()`, or buffer acquisition is performed
while `spectable.lock` is held.  `xcommit` changes ACTIVE to COMMITTING,
waits for borrowers of shadow buffers to leave, then validates and copies
private images through `log_write_real()`.  `xabort` drops the images and
read-set without publishing either.

The current implementation deliberately serializes shadow-map metadata with
one short spinlock.  Buffer data itself is protected by xv6's per-buffer
sleeplock; independent committed buffers do not serialize.  This is a safe
coarse-grained baseline for the later bucket-lock optimization and makes the
remaining scalability limitation explicit.

Version counters are volatile because shadow transactions are volatile: after
a reboot both start from an empty epoch.  First reads are recorded once per
`(transaction, device, block)` and commit rejects an entry if a committed
version has changed.  The selected policy is abort-on-conflict.
