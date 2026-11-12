// Private block versions used by speculative file-system operations.
//
// The table is deliberately bounded.  Bucket locks protect independent hash
// chains and entry pins; the table lock protects the free list and global
// counters.  Entry data has its own lock so unrelated private blocks can be
// copied concurrently.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "fs.h"
#include "shadow.h"

#define NSPEC 16
#define NSHADOW_BUCKETS 16

enum shadow_state {
  SHADOW_FREE = 0,
  SHADOW_ALLOCATED
};

struct shadow_entry {
  int spec_id;
  uint dev;
  uint blockno;
  enum shadow_state state;
  uint refs;
  int next;
  uint base_version;
  char base[BSIZE];
  char data[BSIZE];
  // Chain metadata and pins are protected by the owning bucket lock.
  struct spinlock data_lock;
};

struct reservation_entry {
  int spec_id;
  int kind;
  uint dev;
  uint number;
  uint version;
  int state;
  int used;
};

// A read-only block still participates in optimistic validation.  Keeping
// this separate from shadow_entry avoids allocating a private block merely
// because a transaction read it.
struct read_entry {
  int spec_id;
  uint dev;
  uint blockno;
  uint version;
  int used;
};

static struct {
  struct spinlock lock;
  struct spinlock stats_lock;
  struct shadow_entry entry[NSHADOW];
  struct spinlock bucket_lock[NSHADOW_BUCKETS];
  int head[NSHADOW_BUCKETS];
  int free_head;
  uint allocated;
  uint peak;
  uint spec_count[NSPEC];
  uint bucket_acquires;
  uint bucket_contention;
  uint data_acquires;
  uint lookups;
  uint lookup_hits;
  uint lookup_misses;
  uint allocation_failures;
  int frozen_id[NSPEC];
  uint snapshots[NSPEC];
  uint base_version[NDEV][FSSIZE];
  struct read_entry reads[NSPEC_RESERVATIONS];
  int scope_id[NSPEC];
  int parent_id[NSPEC];
} shadow_table;

static struct {
  struct spinlock lock;
  struct reservation_entry entry[NSPEC_RESERVATIONS];
  uint next_version;
} reservations;

static int reservation_find_locked(int, int, uint, uint);

#define SHADOW_TRACE(event, id, dev, block, expected, actual) \
  spec_trace(event, id, -1, dev, block, expected, actual)

static int
shadow_slot(int spec_id)
{
  int slot;

  if(spec_id <= 0)
    return -1;
  slot = (spec_id & 31) - 1;
  if(slot < 0 || slot >= NSPEC)
    return -1;
  return slot;
}

static uint
shadow_hash(int spec_id, uint dev, uint blockno)
{
  uint key;

  key = (uint)spec_id * 16777619U;
  key ^= dev * 2166136261U;
  key ^= blockno * 709607U;
  return key % NSHADOW_BUCKETS;
}

static void
shadow_bucket_acquire(int bucket)
{
#ifdef SPEC_COARSE_SHADOW_LOCK
  bucket = 0;
#endif
  acquire(&shadow_table.stats_lock);
  shadow_table.bucket_acquires++;
  if(shadow_table.bucket_lock[bucket].locked)
    shadow_table.bucket_contention++;
  release(&shadow_table.stats_lock);
  acquire(&shadow_table.bucket_lock[bucket]);
}

static void
shadow_bucket_release(int bucket)
{
#ifdef SPEC_COARSE_SHADOW_LOCK
  bucket = 0;
#endif
  release(&shadow_table.bucket_lock[bucket]);
}

static void
shadow_data_acquire(struct shadow_entry *entry)
{
  acquire(&shadow_table.stats_lock);
  shadow_table.data_acquires++;
  release(&shadow_table.stats_lock);
  acquire(&entry->data_lock);
}

static int
shadow_find_locked(int spec_id, uint dev, uint blockno, int *prev)
{
  int index, previous;

  acquire(&shadow_table.stats_lock);
  shadow_table.lookups++;
  release(&shadow_table.stats_lock);
  previous = -1;
  index = shadow_table.head[shadow_hash(spec_id, dev, blockno)];
  while(index >= 0){
    if(shadow_table.entry[index].state == SHADOW_ALLOCATED &&
       shadow_table.entry[index].spec_id == spec_id &&
       shadow_table.entry[index].dev == dev &&
       shadow_table.entry[index].blockno == blockno){
      if(prev)
        *prev = previous;
      acquire(&shadow_table.stats_lock);
      shadow_table.lookup_hits++;
      release(&shadow_table.stats_lock);
      return index;
    }
    previous = index;
    index = shadow_table.entry[index].next;
  }
  if(prev)
    *prev = previous;
  acquire(&shadow_table.stats_lock);
  shadow_table.lookup_misses++;
  release(&shadow_table.stats_lock);
  return -1;
}

static int
shadow_count_locked(int spec_id)
{
  int slot;

  slot = shadow_slot(spec_id);
  return slot < 0 ? -1 : shadow_table.spec_count[slot];
}

void
shadowinit(void)
{
  int i;

  initlock(&shadow_table.lock, "shadow");
  initlock(&shadow_table.stats_lock, "shadowstats");
  initlock(&reservations.lock, "reservations");
  shadow_table.free_head = 0;
  shadow_table.allocated = 0;
  shadow_table.peak = 0;
  memset(shadow_table.spec_count, 0, sizeof(shadow_table.spec_count));
  shadow_table.bucket_acquires = 0;
  shadow_table.bucket_contention = 0;
  shadow_table.data_acquires = 0;
  shadow_table.lookups = 0;
  shadow_table.lookup_hits = 0;
  shadow_table.lookup_misses = 0;
  shadow_table.allocation_failures = 0;
  memset(shadow_table.frozen_id, 0, sizeof(shadow_table.frozen_id));
  memset(shadow_table.snapshots, 0, sizeof(shadow_table.snapshots));
  memset(shadow_table.base_version, 0, sizeof(shadow_table.base_version));
  memset(shadow_table.reads, 0, sizeof(shadow_table.reads));
  memset(shadow_table.scope_id, 0, sizeof(shadow_table.scope_id));
  memset(shadow_table.parent_id, 0, sizeof(shadow_table.parent_id));
  memset(reservations.entry, 0, sizeof(reservations.entry));
  reservations.next_version = 1;
  for(i = 0; i < NSHADOW_BUCKETS; i++){
    initlock(&shadow_table.bucket_lock[i], "shadowbucket");
    shadow_table.head[i] = -1;
  }
  for(i = 0; i < NSHADOW; i++){
    memset(&shadow_table.entry[i], 0, sizeof(shadow_table.entry[i]));
    initlock(&shadow_table.entry[i].data_lock, "shadowdata");
    shadow_table.entry[i].state = SHADOW_FREE;
    shadow_table.entry[i].next = i + 1;
  }
  shadow_table.entry[NSHADOW - 1].next = -1;
}

void
shadow_set_parent(int spec_id, int parent_id)
{
  int slot;

  if((slot = shadow_slot(spec_id)) < 0)
    return;
  acquire(&shadow_table.lock);
  shadow_table.scope_id[slot] = spec_id;
  shadow_table.parent_id[slot] = parent_id;
  release(&shadow_table.lock);
}

static int
shadow_parent(int spec_id)
{
  int slot, parent;

  if((slot = shadow_slot(spec_id)) < 0)
    return 0;
  acquire(&shadow_table.lock);
  parent = shadow_table.scope_id[slot] == spec_id ?
           shadow_table.parent_id[slot] : 0;
  release(&shadow_table.lock);
  return parent;
}

static int
shadow_is_ancestor(int ancestor_id, int spec_id)
{
  int parent, depth;

  for(depth = 0; depth < NSPEC && spec_id > 0; depth++){
    parent = shadow_parent(spec_id);
    if(parent == ancestor_id)
      return 1;
    spec_id = parent;
  }
  return 0;
}

// Record the version seen on a transaction's first read.  This is called
// while the filesystem admission gate is held, so a successful record and a
// later publish validation have a well-defined committed base to compare.
int
shadow_note_read(int spec_id, uint dev, uint blockno)
{
  int i, free_slot;
  uint version;

  if(spec_id <= 0 || dev >= NDEV || blockno >= FSSIZE)
    return -1;
  acquire(&shadow_table.lock);
  free_slot = -1;
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    if(shadow_table.reads[i].used && shadow_table.reads[i].spec_id == spec_id &&
       shadow_table.reads[i].dev == dev && shadow_table.reads[i].blockno == blockno){
      release(&shadow_table.lock);
      return 0;
    }
    if(!shadow_table.reads[i].used && free_slot < 0)
      free_slot = i;
  }
  if(free_slot < 0){
    release(&shadow_table.lock);
    SHADOW_TRACE("read_set_full", spec_id, dev, blockno, 0, -1);
    return -1;
  }
  shadow_table.reads[free_slot].spec_id = spec_id;
  shadow_table.reads[free_slot].dev = dev;
  shadow_table.reads[free_slot].blockno = blockno;
  shadow_table.reads[free_slot].version = shadow_table.base_version[dev][blockno];
  shadow_table.reads[free_slot].used = 1;
  version = shadow_table.reads[free_slot].version;
  release(&shadow_table.lock);
  SHADOW_TRACE("read_set", spec_id, dev, blockno, 0,
               (int)version);
  return 0;
}

// Return 1 for a private version, 0 when the caller must use the base block,
// and -1 for an invalid or unavailable destination.
int
shadow_read(int spec_id, uint dev, uint blockno, char *dst)
{
  int index, bucket, parent;

  if(spec_id <= 0 || dst == 0)
    return -1;
  bucket = shadow_hash(spec_id, dev, blockno);
  shadow_bucket_acquire(bucket);
  index = shadow_find_locked(spec_id, dev, blockno, 0);
  if(index < 0){
    shadow_bucket_release(bucket);
    parent = shadow_parent(spec_id);
    if(parent != 0)
      return shadow_read(parent, dev, blockno, dst);
    SHADOW_TRACE("block_read", spec_id, dev, blockno, 0, 0);
    return 0;
  }
  shadow_table.entry[index].refs++;
  shadow_bucket_release(bucket);
  shadow_data_acquire(&shadow_table.entry[index]);
  memmove(dst, shadow_table.entry[index].data, BSIZE);
  release(&shadow_table.entry[index].data_lock);
  shadow_bucket_acquire(bucket);
  shadow_table.entry[index].refs--;
  shadow_bucket_release(bucket);
  SHADOW_TRACE("block_read", spec_id, dev, blockno, 0, 1);
  return 1;
}

int
shadow_read_image(int spec_id, uint dev, uint blockno, char *dst)
{
  return shadow_read(spec_id, dev, blockno, dst);
}

// Install or replace a private version.  The first write copies the caller's
// base-block image, so allocation never exposes an uninitialized block.
int
shadow_write(int spec_id, uint dev, uint blockno, const char *src)
{
  return shadow_write_versioned(spec_id, dev, blockno,
                                shadow_base_version(dev, blockno),
                                src, src);
}

int
shadow_write_versioned(int spec_id, uint dev, uint blockno,
                       uint base_version, const char *base, const char *src)
{
  int index, bucket;
  int count;
  int slot;
  int allocated;

  if(spec_id <= 0 || base == 0 || src == 0 ||
     dev >= NDEV || blockno >= FSSIZE || (slot = shadow_slot(spec_id)) < 0)
    return -1;
  allocated = 0;
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != 0){
    release(&shadow_table.lock);
    SHADOW_TRACE("block_write_rejected", spec_id, dev, blockno, 0, -1);
    return -1;
  }
  release(&shadow_table.lock);
  bucket = shadow_hash(spec_id, dev, blockno);
  shadow_bucket_acquire(bucket);
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != 0){
    release(&shadow_table.lock);
    shadow_bucket_release(bucket);
    SHADOW_TRACE("block_write_rejected", spec_id, dev, blockno, 0, -1);
    return -1;
  }
  release(&shadow_table.lock);
  index = shadow_find_locked(spec_id, dev, blockno, 0);
  if(index < 0){
    acquire(&shadow_table.lock);
    count = shadow_count_locked(spec_id);
    if(count >= NSHADOW_QUOTA || shadow_table.free_head < 0){
      acquire(&shadow_table.stats_lock);
      shadow_table.allocation_failures++;
      release(&shadow_table.stats_lock);
      release(&shadow_table.lock);
      shadow_bucket_release(bucket);
      SHADOW_TRACE("shadow_exhausted", spec_id, dev, blockno,
                   NSHADOW_QUOTA, count);
      return -1;
    }
    index = shadow_table.free_head;
    shadow_table.free_head = shadow_table.entry[index].next;
    initlock(&shadow_table.entry[index].data_lock, "shadowdata");
    bucket = shadow_hash(spec_id, dev, blockno);
    shadow_table.entry[index].next = shadow_table.head[bucket];
    shadow_table.head[bucket] = index;
    shadow_table.entry[index].spec_id = spec_id;
    shadow_table.entry[index].dev = dev;
    shadow_table.entry[index].blockno = blockno;
    shadow_table.entry[index].state = SHADOW_ALLOCATED;
    shadow_table.entry[index].refs = 0;
    shadow_table.entry[index].base_version = base_version;
    memmove(shadow_table.entry[index].base, base, BSIZE);
    allocated = 1;
    shadow_table.spec_count[slot]++;
    shadow_table.allocated++;
    if(shadow_table.allocated > shadow_table.peak)
      shadow_table.peak = shadow_table.allocated;
    release(&shadow_table.lock);
  }
  shadow_table.entry[index].refs++;
  shadow_bucket_release(bucket);
  shadow_data_acquire(&shadow_table.entry[index]);
  memmove(shadow_table.entry[index].data, src, BSIZE);
  release(&shadow_table.entry[index].data_lock);
  shadow_bucket_acquire(bucket);
  shadow_table.entry[index].refs--;
  shadow_bucket_release(bucket);
  if(allocated)
    SHADOW_TRACE("block_alloc", spec_id, dev, blockno, 0, (int)base_version);
  SHADOW_TRACE("block_write", spec_id, dev, blockno, 0, (int)base_version);
  return 0;
}

int
shadow_write_image(int spec_id, uint dev, uint blockno, const char *src)
{
  return shadow_write(spec_id, dev, blockno, src);
}

uint
shadow_base_version(uint dev, uint blockno)
{
  uint version;

  if(dev >= NDEV || blockno >= FSSIZE)
    return 0;
  acquire(&shadow_table.lock);
  version = shadow_table.base_version[dev][blockno];
  release(&shadow_table.lock);
  return version;
}

void
shadow_note_base_write(uint dev, uint blockno)
{
  uint version;

  if(dev >= NDEV || blockno >= FSSIZE)
    return;
  acquire(&shadow_table.lock);
  shadow_table.base_version[dev][blockno]++;
  if(shadow_table.base_version[dev][blockno] == 0)
    shadow_table.base_version[dev][blockno] = 1;
  version = shadow_table.base_version[dev][blockno];
  release(&shadow_table.lock);
  SHADOW_TRACE("base_write", 0, dev, blockno, 0, (int)version);
}

int
shadow_validate_versions(int spec_id)
{
  int i, bucket;
  uint version;
  uint dev, blockno, expected;

  for(bucket = 0; bucket < NSHADOW_BUCKETS; bucket++){
    shadow_bucket_acquire(bucket);
    for(i = 0; i < NSHADOW; i++){
      struct shadow_entry *e = &shadow_table.entry[i];
      if(e->state != SHADOW_ALLOCATED || e->spec_id != spec_id ||
         shadow_hash(e->spec_id, e->dev, e->blockno) != (uint)bucket)
        continue;
      acquire(&shadow_table.lock);
      version = shadow_table.base_version[e->dev][e->blockno];
      if(version != e->base_version){
        dev = e->dev;
        blockno = e->blockno;
        expected = e->base_version;
        release(&shadow_table.lock);
        shadow_bucket_release(bucket);
        SHADOW_TRACE("write_conflict", spec_id, dev, blockno,
                     (int)expected, (int)version);
        return -1;
      }
      release(&shadow_table.lock);
    }
    shadow_bucket_release(bucket);
  }
  acquire(&shadow_table.lock);
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    struct read_entry *r = &shadow_table.reads[i];
    if(r->used && r->spec_id == spec_id){
      version = shadow_table.base_version[r->dev][r->blockno];
      if(version != r->version){
        dev = r->dev;
        blockno = r->blockno;
        expected = r->version;
        release(&shadow_table.lock);
        SHADOW_TRACE("read_conflict", spec_id, dev, blockno,
                     (int)expected, (int)version);
        return -1;
      }
    }
  }
  release(&shadow_table.lock);
  return 0;
}

// Merge a frozen nested overlay into its frozen parent. Both contexts must
// already have drained operations and been frozen by the lifecycle layer.
// All bucket locks are held before metadata, matching the ordinary bucket ->
// metadata lock order. No allocation is needed: duplicate child blocks reuse
// their slot, while child-only slots are re-keyed into the parent's bucket.
int
shadow_merge(int child_id, int parent_id)
{
  int i, b, slot, pslot, child_count, parent_count, extra;
  int parent_index, previous, index, next;

  slot = shadow_slot(child_id);
  pslot = shadow_slot(parent_id);
  if(slot < 0 || pslot < 0 || slot == pslot)
    return -1;
  for(b = 0; b < NSHADOW_BUCKETS; b++)
    acquire(&shadow_table.bucket_lock[b]);
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != child_id ||
     shadow_table.frozen_id[pslot] != parent_id ||
     shadow_table.scope_id[slot] != child_id ||
     shadow_table.parent_id[slot] != parent_id){
    release(&shadow_table.lock);
    for(b = NSHADOW_BUCKETS - 1; b >= 0; b--)
      release(&shadow_table.bucket_lock[b]);
    return -1;
  }

  child_count = shadow_table.spec_count[slot];
  parent_count = shadow_table.spec_count[pslot];
  extra = 0;
  for(i = 0; i < NSHADOW; i++){
    struct shadow_entry *e = &shadow_table.entry[i];
    if(e->state == SHADOW_ALLOCATED && e->spec_id == child_id &&
       shadow_find_locked(parent_id, e->dev, e->blockno, 0) < 0)
      extra++;
  }
  if(parent_count + extra > NSHADOW_QUOTA){
    release(&shadow_table.lock);
    for(b = NSHADOW_BUCKETS - 1; b >= 0; b--)
      release(&shadow_table.bucket_lock[b]);
    return -1;
  }

  for(i = 0; i < NSHADOW; i++){
    struct shadow_entry *e = &shadow_table.entry[i];
    if(e->state != SHADOW_ALLOCATED || e->spec_id != child_id)
      continue;
    b = shadow_hash(child_id, e->dev, e->blockno);
    previous = -1;
    index = shadow_table.head[b];
    while(index >= 0 && index != i){
      previous = index;
      index = shadow_table.entry[index].next;
    }
    if(index != i)
      panic("shadow_merge: missing child entry");
    next = e->next;
    if(previous < 0)
      shadow_table.head[b] = next;
    else
      shadow_table.entry[previous].next = next;

    parent_index = shadow_find_locked(parent_id, e->dev, e->blockno, 0);
    if(parent_index >= 0){
      acquire(&shadow_table.entry[parent_index].data_lock);
      acquire(&e->data_lock);
      memmove(shadow_table.entry[parent_index].data, e->data, BSIZE);
      release(&e->data_lock);
      release(&shadow_table.entry[parent_index].data_lock);
      e->state = SHADOW_FREE;
      e->spec_id = 0;
      e->next = shadow_table.free_head;
      shadow_table.free_head = i;
      shadow_table.spec_count[slot]--;
      shadow_table.allocated--;
    } else {
      e->spec_id = parent_id;
      b = shadow_hash(parent_id, e->dev, e->blockno);
      e->next = shadow_table.head[b];
      shadow_table.head[b] = i;
      shadow_table.spec_count[slot]--;
      shadow_table.spec_count[pslot]++;
    }
  }
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    struct read_entry *r = &shadow_table.reads[i];
    if(!r->used || r->spec_id != child_id)
      continue;
    for(b = 0; b < NSPEC_RESERVATIONS; b++){
      struct read_entry *p = &shadow_table.reads[b];
      if(p->used && p->spec_id == parent_id && p->dev == r->dev &&
         p->blockno == r->blockno)
        break;
    }
    if(b < NSPEC_RESERVATIONS)
      r->used = 0;
    else
      r->spec_id = parent_id;
  }
  shadow_table.frozen_id[slot] = 0;
  shadow_table.scope_id[slot] = 0;
  shadow_table.parent_id[slot] = 0;
  release(&shadow_table.lock);
  for(b = NSHADOW_BUCKETS - 1; b >= 0; b--)
    release(&shadow_table.bucket_lock[b]);

  acquire(&reservations.lock);
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    struct reservation_entry *e = &reservations.entry[i];
    int j;
    if(!e->used || e->spec_id != child_id)
      continue;
    j = reservation_find_locked(parent_id, e->kind, e->dev, e->number);
    if(j >= 0){
      reservations.entry[j].state = e->state;
      reservations.entry[j].version = e->version;
      memset(e, 0, sizeof(*e));
    } else
      e->spec_id = parent_id;
  }
  release(&reservations.lock);
  (void)child_count;
  return 0;
}

int
shadow_freeze(int spec_id)
{
  int slot;

  if((slot = shadow_slot(spec_id)) < 0)
    return -1;
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != 0){
    release(&shadow_table.lock);
    return -1;
  }
  shadow_table.frozen_id[slot] = spec_id;
  release(&shadow_table.lock);
  return 0;
}

int
shadow_thaw(int spec_id)
{
  int slot;

  if((slot = shadow_slot(spec_id)) < 0)
    return -1;
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != spec_id ||
     shadow_table.snapshots[slot] != 0){
    release(&shadow_table.lock);
    return -1;
  }
  shadow_table.frozen_id[slot] = 0;
  release(&shadow_table.lock);
  return 0;
}

int
shadow_snapshot_begin(int spec_id, struct shadow_snapshot *snap)
{
  int slot;

  if(snap == 0 || (slot = shadow_slot(spec_id)) < 0)
    return -1;
  memset(snap, 0, sizeof(*snap));
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != spec_id){
    release(&shadow_table.lock);
    return -1;
  }
  snap->spec_id = spec_id;
  snap->count = shadow_count_locked(spec_id);
  snap->active = 1;
  shadow_table.snapshots[slot]++;
  release(&shadow_table.lock);
  return 0;
}

int
shadow_snapshot_next(struct shadow_snapshot *snap, struct shadow_block *block)
{
  int i, bucket, seen;

  if(snap == 0 || block == 0 || !snap->active)
    return -1;
  seen = 0;
  for(bucket = 0; bucket < NSHADOW_BUCKETS; bucket++){
    shadow_bucket_acquire(bucket);
    for(i = 0; i < NSHADOW; i++){
      if(shadow_table.entry[i].state != SHADOW_ALLOCATED ||
         shadow_table.entry[i].spec_id != snap->spec_id ||
         shadow_hash(shadow_table.entry[i].spec_id,
                     shadow_table.entry[i].dev,
                     shadow_table.entry[i].blockno) != (uint)bucket)
        continue;
      if(seen++ < snap->cursor)
        continue;
      block->dev = shadow_table.entry[i].dev;
      block->blockno = shadow_table.entry[i].blockno;
      snap->cursor++;
      shadow_table.entry[i].refs++;
      shadow_bucket_release(bucket);
      shadow_data_acquire(&shadow_table.entry[i]);
      memmove(block->data, shadow_table.entry[i].data, BSIZE);
      release(&shadow_table.entry[i].data_lock);
      shadow_bucket_acquire(bucket);
      shadow_table.entry[i].refs--;
      shadow_bucket_release(bucket);
      return 1;
    }
    shadow_bucket_release(bucket);
  }
  return 0;
}

void
shadow_snapshot_end(struct shadow_snapshot *snap)
{
  int slot;

  if(snap == 0 || !snap->active)
    panic("shadow_snapshot_end");
  slot = shadow_slot(snap->spec_id);
  acquire(&shadow_table.lock);
  if(slot < 0 || shadow_table.snapshots[slot] == 0){
    release(&shadow_table.lock);
    panic("shadow_snapshot_end state");
  }
  shadow_table.snapshots[slot]--;
  release(&shadow_table.lock);
  memset(snap, 0, sizeof(*snap));
}

// Validate private-set structure only. Base-version conflict validation is a
// publication responsibility until the block layer records base versions.
int
shadow_validate(int spec_id)
{
  int i, bucket, slot, count;

  if(spec_id <= 0)
    return -1;
  if((slot = shadow_slot(spec_id)) < 0)
    return -1;
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] != spec_id){
    release(&shadow_table.lock);
    return -1;
  }
  count = shadow_count_locked(spec_id);
  if(count > NSHADOW_QUOTA){
    release(&shadow_table.lock);
    return -1;
  }
  release(&shadow_table.lock);
  // The hash insertion path forbids duplicate keys; verify that invariant
  // while the frozen set is stable before handing it to a publisher.
  for(bucket = 0; bucket < NSHADOW_BUCKETS; bucket++){
    shadow_bucket_acquire(bucket);
    for(i = 0; i < NSHADOW; i++){
      struct shadow_entry *e = &shadow_table.entry[i];
      if(e->state == SHADOW_ALLOCATED && e->spec_id == spec_id &&
         shadow_hash(e->spec_id, e->dev, e->blockno) == (uint)bucket &&
         shadow_find_locked(spec_id, e->dev, e->blockno, 0) != i){
        shadow_bucket_release(bucket);
        return -1;
      }
    }
    shadow_bucket_release(bucket);
  }
  return 0;
}

void
shadow_discard(int spec_id)
{
  int i, index, previous, next;
  int slot;

  if(spec_id <= 0 || (slot = shadow_slot(spec_id)) < 0)
    return;
  acquire(&shadow_table.lock);
  if(shadow_table.snapshots[slot] != 0){
    release(&shadow_table.lock);
    panic("shadow_discard: snapshot active");
  }
  release(&shadow_table.lock);
  for(i = 0; i < NSHADOW_BUCKETS; i++){
    shadow_bucket_acquire(i);
    previous = -1;
    index = shadow_table.head[i];
    while(index >= 0){
      next = shadow_table.entry[index].next;
      if(shadow_table.entry[index].state == SHADOW_ALLOCATED &&
         shadow_table.entry[index].spec_id == spec_id){
        if(shadow_table.entry[index].refs != 0)
          panic("shadow_discard: pinned");
        if(previous < 0)
          shadow_table.head[i] = next;
        else
          shadow_table.entry[previous].next = next;
        acquire(&shadow_table.lock);
        memset(&shadow_table.entry[index], 0,
               sizeof(shadow_table.entry[index]));
        shadow_table.entry[index].state = SHADOW_FREE;
        shadow_table.entry[index].next = shadow_table.free_head;
        shadow_table.free_head = index;
        shadow_table.allocated--;
        if(shadow_table.spec_count[slot] == 0)
          panic("shadow_discard: count");
        shadow_table.spec_count[slot]--;
        release(&shadow_table.lock);
      } else {
        previous = index;
      }
      index = next;
    }
    shadow_bucket_release(i);
  }
  acquire(&shadow_table.lock);
  if(shadow_table.frozen_id[slot] == spec_id)
    shadow_table.frozen_id[slot] = 0;
  if(shadow_table.scope_id[slot] == spec_id){
    shadow_table.scope_id[slot] = 0;
    shadow_table.parent_id[slot] = 0;
  }
  for(i = 0; i < NSPEC_RESERVATIONS; i++)
    if(shadow_table.reads[i].used && shadow_table.reads[i].spec_id == spec_id)
      memset(&shadow_table.reads[i], 0, sizeof(shadow_table.reads[i]));
  release(&shadow_table.lock);
  spec_reservation_discard(spec_id);
}

int
shadow_count(int spec_id)
{
  int count;

  if(spec_id <= 0)
    return -1;
  acquire(&shadow_table.lock);
  count = shadow_count_locked(spec_id);
  release(&shadow_table.lock);
  return count;
}

int
shadow_capacity(void)
{
  return NSHADOW;
}

int
shadow_peak(void)
{
  int peak;

  acquire(&shadow_table.lock);
  peak = shadow_table.peak;
  release(&shadow_table.lock);
  return peak;
}

void
shadow_lock_stats_get(struct shadow_lock_stats *stats)
{
  if(stats == 0)
    return;
  acquire(&shadow_table.stats_lock);
  stats->bucket_acquires = shadow_table.bucket_acquires;
  stats->bucket_contention = shadow_table.bucket_contention;
  stats->data_acquires = shadow_table.data_acquires;
  release(&shadow_table.stats_lock);
}

void
shadow_metrics_get(struct shadow_metrics *stats)
{
  if(stats == 0)
    return;
  acquire(&shadow_table.stats_lock);
  stats->bucket_acquires = shadow_table.bucket_acquires;
  stats->bucket_contention = shadow_table.bucket_contention;
  stats->data_acquires = shadow_table.data_acquires;
  stats->lookups = shadow_table.lookups;
  stats->lookup_hits = shadow_table.lookup_hits;
  stats->lookup_misses = shadow_table.lookup_misses;
  stats->allocation_failures = shadow_table.allocation_failures;
  release(&shadow_table.stats_lock);
  acquire(&shadow_table.lock);
  stats->allocated = shadow_table.allocated;
  stats->peak = shadow_table.peak;
  release(&shadow_table.lock);
  stats->capacity = NSHADOW;
}

static int
reservation_find_locked(int spec_id, int kind, uint dev, uint number)
{
  int i;

  for(i = 0; i < NSPEC_RESERVATIONS; i++)
    if(reservations.entry[i].used &&
       reservations.entry[i].spec_id == spec_id &&
       reservations.entry[i].kind == kind &&
       reservations.entry[i].dev == dev &&
       reservations.entry[i].number == number)
      return i;
  return -1;
}

int
spec_reservation_claim(int spec_id, int kind, uint dev, uint number,
                       int state, struct spec_reservation_token *token)
{
  int i, free_slot, previous, inherited;
  uint previous_version;

  if(token == 0 || spec_id < 0 || (kind != SPEC_RES_INODE &&
     kind != SPEC_RES_BLOCK) || (state != SPEC_RES_ALLOCATED &&
     state != SPEC_RES_DEFERRED_FREE))
    return -1;
  memset(token, 0, sizeof(*token));
  inherited = 0;
  acquire(&reservations.lock);
  free_slot = -1;
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    struct reservation_entry *e = &reservations.entry[i];
    if(!e->used){
      if(free_slot < 0)
        free_slot = i;
      continue;
    }
    if(e->kind == kind && e->dev == dev && e->number == number){
      if(e->spec_id == spec_id && spec_id > 0){
        free_slot = i;
        break;
      }
      if(spec_id > 0 && shadow_is_ancestor(e->spec_id, spec_id)){
        inherited = 1;
        continue;
      }
      if(e->spec_id != spec_id || spec_id == 0){
        release(&reservations.lock);
        return -1;
      }
    }
  }
  if(inherited && free_slot >= 0 && reservations.entry[free_slot].used)
    free_slot = -1;
  if(free_slot < 0){
    release(&reservations.lock);
    return -1;
  }
  previous = reservations.entry[free_slot].used ?
             reservations.entry[free_slot].state : 0;
  previous_version = reservations.entry[free_slot].used ?
                     reservations.entry[free_slot].version : 0;
  reservations.entry[free_slot].used = 1;
  reservations.entry[free_slot].spec_id = spec_id;
  reservations.entry[free_slot].kind = kind;
  reservations.entry[free_slot].dev = dev;
  reservations.entry[free_slot].number = number;
  reservations.entry[free_slot].state = state;
  reservations.entry[free_slot].version = reservations.next_version++;
  if(reservations.entry[free_slot].version == 0){
    reservations.entry[free_slot].version = reservations.next_version++;
    if(reservations.entry[free_slot].version == 0)
      reservations.entry[free_slot].version = 1;
  }
  token->spec_id = spec_id;
  token->kind = kind;
  token->dev = dev;
  token->number = number;
  token->previous_version = previous_version;
  token->installed_version = reservations.entry[free_slot].version;
  token->previous_state = previous;
  token->installed_state = state;
  token->active = 1;
  release(&reservations.lock);
  return 0;
}

int
spec_reservation_check(int spec_id, int kind, uint dev, uint number)
{
  int i, available;

  if(spec_id < 0 || (kind != SPEC_RES_INODE && kind != SPEC_RES_BLOCK))
    return 0;
  acquire(&reservations.lock);
  available = 1;
  for(i = 0; i < NSPEC_RESERVATIONS; i++){
    struct reservation_entry *e = &reservations.entry[i];
    if(e->used && e->kind == kind && e->dev == dev &&
       e->number == number &&
       ((e->spec_id != spec_id &&
         !(spec_id > 0 && shadow_is_ancestor(e->spec_id, spec_id))) ||
        spec_id == 0)){
      available = 0;
      break;
    }
  }
  release(&reservations.lock);
  return available;
}

int
spec_reservation_state(int spec_id, int kind, uint dev, uint number)
{
  int i, state, current, depth;

  if(spec_id < 0)
    return -1;
  acquire(&reservations.lock);
  i = reservation_find_locked(spec_id, kind, dev, number);
  if(i >= 0)
    state = reservations.entry[i].state;
  else {
    state = 0;
    current = spec_id;
    for(depth = 0; depth < NSPEC && current > 0 && state == 0; depth++){
      current = shadow_parent(current);
      if(current == 0)
        break;
      i = reservation_find_locked(current, kind, dev, number);
      if(i >= 0)
        state = reservations.entry[i].state;
    }
    if(state == 0)
      for(i = 0; i < NSPEC_RESERVATIONS; i++)
        if(reservations.entry[i].used && reservations.entry[i].kind == kind &&
           reservations.entry[i].dev == dev &&
           reservations.entry[i].number == number){
          state = -1;
          break;
        }
  }
  release(&reservations.lock);
  return state;
}

static void
reservation_restore_locked(struct spec_reservation_token *token)
{
  int i;

  i = reservation_find_locked(token->spec_id, token->kind,
                              token->dev, token->number);
  if(i < 0 || reservations.entry[i].state != token->installed_state ||
     reservations.entry[i].version != token->installed_version)
    return;
  if(token->previous_state == 0)
    memset(&reservations.entry[i], 0, sizeof(reservations.entry[i]));
  else {
    reservations.entry[i].state = token->previous_state;
    reservations.entry[i].version = token->previous_version;
  }
}

void
spec_reservation_cancel(struct spec_reservation_token *token)
{
  if(token == 0 || !token->active)
    return;
  acquire(&reservations.lock);
  reservation_restore_locked(token);
  token->active = 0;
  release(&reservations.lock);
}

void
spec_reservation_finish(struct spec_reservation_token *token)
{
  int i;

  if(token == 0 || !token->active)
    return;
  acquire(&reservations.lock);
  if(token->spec_id == 0){
    i = reservation_find_locked(0, token->kind, token->dev, token->number);
    if(i >= 0 && reservations.entry[i].state == token->installed_state &&
       reservations.entry[i].version == token->installed_version)
      memset(&reservations.entry[i], 0, sizeof(reservations.entry[i]));
  }
  token->active = 0;
  release(&reservations.lock);
}

static void
reservation_remove_context(int spec_id)
{
  int i;

  if(spec_id <= 0)
    return;
  acquire(&reservations.lock);
  for(i = 0; i < NSPEC_RESERVATIONS; i++)
    if(reservations.entry[i].used &&
       reservations.entry[i].spec_id == spec_id)
      memset(&reservations.entry[i], 0, sizeof(reservations.entry[i]));
  release(&reservations.lock);
}

void
spec_reservation_discard(int spec_id)
{
  reservation_remove_context(spec_id);
}
