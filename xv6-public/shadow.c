// Private block versions used by speculative file-system operations.
//
// The table is deliberately bounded.  A single lock protects the hash chains,
// free-list metadata, reference counts and block contents.  The hash index
// avoids repeated full-table scans while retaining a simple lock order for
// this initial storage layer.  Later scalability work can split this lock by
// bucket without changing the public API.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "fs.h"
#include "shadow.h"

#define NSPEC 16

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
  // Metadata is protected by shadow_table.lock; the private image uses an
  // independent lock so unrelated speculative blocks can be copied in
  // parallel after a reference has pinned their entries.
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
  struct shadow_entry entry[NSHADOW];
  int head[NSHADOW];
  int free_head;
  uint allocated;
  uint peak;
  int frozen_id[NSPEC];
  uint snapshots[NSPEC];
  uint base_version[NDEV][FSSIZE];
  struct read_entry reads[NSPEC_RESERVATIONS];
} shadow_table;

static struct {
  struct spinlock lock;
  struct reservation_entry entry[NSPEC_RESERVATIONS];
  uint next_version;
} reservations;

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
  return key % NSHADOW;
}

static int
shadow_find_locked(int spec_id, uint dev, uint blockno, int *prev)
{
  int index, previous;

  previous = -1;
  index = shadow_table.head[shadow_hash(spec_id, dev, blockno)];
  while(index >= 0){
    if(shadow_table.entry[index].state == SHADOW_ALLOCATED &&
       shadow_table.entry[index].spec_id == spec_id &&
       shadow_table.entry[index].dev == dev &&
       shadow_table.entry[index].blockno == blockno){
      if(prev)
        *prev = previous;
      return index;
    }
    previous = index;
    index = shadow_table.entry[index].next;
  }
  if(prev)
    *prev = previous;
  return -1;
}

static int
shadow_count_locked(int spec_id)
{
  int i, count;

  count = 0;
  for(i = 0; i < NSHADOW; i++)
    if(shadow_table.entry[i].state == SHADOW_ALLOCATED &&
       shadow_table.entry[i].spec_id == spec_id)
      count++;
  return count;
}

void
shadowinit(void)
{
  int i;

  initlock(&shadow_table.lock, "shadow");
  initlock(&reservations.lock, "reservations");
  shadow_table.free_head = 0;
  shadow_table.allocated = 0;
  shadow_table.peak = 0;
  memset(shadow_table.frozen_id, 0, sizeof(shadow_table.frozen_id));
  memset(shadow_table.snapshots, 0, sizeof(shadow_table.snapshots));
  memset(shadow_table.base_version, 0, sizeof(shadow_table.base_version));
  memset(shadow_table.reads, 0, sizeof(shadow_table.reads));
  memset(reservations.entry, 0, sizeof(reservations.entry));
  reservations.next_version = 1;
  for(i = 0; i < NSHADOW; i++){
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
  int index;

  if(spec_id <= 0 || dst == 0)
    return -1;
  acquire(&shadow_table.lock);
  index = shadow_find_locked(spec_id, dev, blockno, 0);
  if(index < 0){
    release(&shadow_table.lock);
    SHADOW_TRACE("block_read", spec_id, dev, blockno, 0, 0);
    return 0;
  }
  shadow_table.entry[index].refs++;
  release(&shadow_table.lock);
  acquire(&shadow_table.entry[index].data_lock);
  memmove(dst, shadow_table.entry[index].data, BSIZE);
  release(&shadow_table.entry[index].data_lock);
  acquire(&shadow_table.lock);
  shadow_table.entry[index].refs--;
  release(&shadow_table.lock);
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
  index = shadow_find_locked(spec_id, dev, blockno, 0);
  if(index < 0){
    count = shadow_count_locked(spec_id);
    if(count >= NSHADOW_QUOTA || shadow_table.free_head < 0){
      release(&shadow_table.lock);
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
    shadow_table.allocated++;
    if(shadow_table.allocated > shadow_table.peak)
      shadow_table.peak = shadow_table.allocated;
  }
  shadow_table.entry[index].refs++;
  release(&shadow_table.lock);
  acquire(&shadow_table.entry[index].data_lock);
  memmove(shadow_table.entry[index].data, src, BSIZE);
  release(&shadow_table.entry[index].data_lock);
  acquire(&shadow_table.lock);
  shadow_table.entry[index].refs--;
  release(&shadow_table.lock);
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
  int i;
  uint version;
  uint dev, blockno, expected;

  acquire(&shadow_table.lock);
  for(i = 0; i < NSHADOW; i++){
    struct shadow_entry *e = &shadow_table.entry[i];
    if(e->state == SHADOW_ALLOCATED && e->spec_id == spec_id){
      version = shadow_table.base_version[e->dev][e->blockno];
      if(version != e->base_version){
        dev = e->dev;
        blockno = e->blockno;
        expected = e->base_version;
        release(&shadow_table.lock);
        SHADOW_TRACE("write_conflict", spec_id, dev, blockno,
                     (int)expected, (int)version);
        return -1;
      }
    }
  }
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
  int i, seen;

  if(snap == 0 || block == 0 || !snap->active)
    return -1;
  acquire(&shadow_table.lock);
  seen = 0;
  for(i = 0; i < NSHADOW; i++){
    if(shadow_table.entry[i].state != SHADOW_ALLOCATED ||
       shadow_table.entry[i].spec_id != snap->spec_id)
      continue;
    if(seen++ < snap->cursor)
      continue;
    block->dev = shadow_table.entry[i].dev;
    block->blockno = shadow_table.entry[i].blockno;
    snap->cursor++;
    shadow_table.entry[i].refs++;
    release(&shadow_table.lock);
    acquire(&shadow_table.entry[i].data_lock);
    memmove(block->data, shadow_table.entry[i].data, BSIZE);
    release(&shadow_table.entry[i].data_lock);
    acquire(&shadow_table.lock);
    shadow_table.entry[i].refs--;
    release(&shadow_table.lock);
    return 1;
  }
  release(&shadow_table.lock);
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
  int i, slot, count;

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
  // The hash insertion path forbids duplicate keys; verify that invariant
  // while the frozen set is stable before handing it to a publisher.
  for(i = 0; i < NSHADOW; i++){
    struct shadow_entry *e = &shadow_table.entry[i];
    if(e->state == SHADOW_ALLOCATED && e->spec_id == spec_id &&
       shadow_find_locked(spec_id, e->dev, e->blockno, 0) != i){
      release(&shadow_table.lock);
      return -1;
    }
  }
  release(&shadow_table.lock);
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
  for(i = 0; i < NSHADOW; i++){
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
        memset(&shadow_table.entry[index], 0,
               sizeof(shadow_table.entry[index]));
        shadow_table.entry[index].state = SHADOW_FREE;
        shadow_table.entry[index].next = shadow_table.free_head;
        shadow_table.free_head = index;
        shadow_table.allocated--;
      } else {
        previous = index;
      }
      index = next;
    }
  }
  if(shadow_table.frozen_id[slot] == spec_id)
    shadow_table.frozen_id[slot] = 0;
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
  int i, free_slot, previous;
  uint previous_version;

  if(token == 0 || spec_id < 0 || (kind != SPEC_RES_INODE &&
     kind != SPEC_RES_BLOCK) || (state != SPEC_RES_ALLOCATED &&
     state != SPEC_RES_DEFERRED_FREE))
    return -1;
  memset(token, 0, sizeof(*token));
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
      if(e->spec_id != spec_id || spec_id == 0){
        release(&reservations.lock);
        return -1;
      }
      free_slot = i;
      break;
    }
  }
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
       (e->spec_id != spec_id || spec_id == 0)){
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
  int i, state;

  if(spec_id < 0)
    return -1;
  acquire(&reservations.lock);
  i = reservation_find_locked(spec_id, kind, dev, number);
  if(i >= 0)
    state = reservations.entry[i].state;
  else {
    state = 0;
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
