# Speculation locking and validation

The implementation uses the following lock hierarchy:

1. `spectable.lock` protects context state, process references and the
   in-flight buffer-operation count.
2. `shadow_table.lock` protects the free list, global occupancy counters,
   freeze state, version counters and deduplicated read-set. Each hash bucket
   has an independent lock for its chain and entry lifetime pins. Each
   allocated entry has a separate `data_lock` protecting its private block
   image.
3. `versions.lock` protects committed block-version counters.
4. xv6 buffer sleeplocks and the existing log lock retain their normal roles.

No disk I/O, `begin_op()`, `end_op()`, or buffer acquisition is performed
while `spectable.lock` is held.  `xcommit` changes ACTIVE to COMMITTING,
waits for borrowers of shadow buffers to leave, then validates and copies
private images through `log_write_real()`.  `xabort` drops the images and
read-set without publishing either.

Shadow lookup, insertion, snapshot, and reclamation acquire only the
corresponding bucket lock. Allocation briefly acquires the global metadata
lock for the free list and counters while retaining the bucket lock; the
required order is bucket lock before metadata lock. A lookup pins its entry,
releases the bucket lock, and copies under the entry's `data_lock`;
reclamation checks the pin count before reuse. Consequently independent
shadow buckets and private block images can be read or updated concurrently.
`shadow_lock_stats_get()` exposes bucket acquisitions, observed contention,
and data-lock acquisitions for workload instrumentation.

Version counters are volatile because shadow transactions are volatile: after
a reboot both start from an empty epoch.  First reads are recorded once per
`(transaction, device, block)` and commit rejects an entry if a committed
version has changed.  The selected policy is abort-on-conflict.
