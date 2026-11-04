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
  char data[BSIZE];
};

static struct {
  struct spinlock lock;
  struct shadow_entry entry[NSHADOW];
  int head[NSHADOW];
  int free_head;
  uint allocated;
  uint peak;
} shadow_table;

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
  shadow_table.free_head = 0;
  shadow_table.allocated = 0;
  shadow_table.peak = 0;
  for(i = 0; i < NSHADOW; i++){
    shadow_table.head[i] = -1;
  }
  for(i = 0; i < NSHADOW; i++){
    memset(&shadow_table.entry[i], 0, sizeof(shadow_table.entry[i]));
    shadow_table.entry[i].state = SHADOW_FREE;
    shadow_table.entry[i].next = i + 1;
  }
  shadow_table.entry[NSHADOW - 1].next = -1;
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
    return 0;
  }
  shadow_table.entry[index].refs++;
  memmove(dst, shadow_table.entry[index].data, BSIZE);
  shadow_table.entry[index].refs--;
  release(&shadow_table.lock);
  return 1;
}

// Install or replace a private version.  The first write copies the caller's
// base-block image, so allocation never exposes an uninitialized block.
int
shadow_write(int spec_id, uint dev, uint blockno, const char *src)
{
  int index, bucket;
  int count;

  if(spec_id <= 0 || src == 0)
    return -1;
  acquire(&shadow_table.lock);
  index = shadow_find_locked(spec_id, dev, blockno, 0);
  if(index < 0){
    count = shadow_count_locked(spec_id);
    if(count >= NSHADOW_QUOTA || shadow_table.free_head < 0){
      release(&shadow_table.lock);
      return -1;
    }
    index = shadow_table.free_head;
    shadow_table.free_head = shadow_table.entry[index].next;
    bucket = shadow_hash(spec_id, dev, blockno);
    shadow_table.entry[index].next = shadow_table.head[bucket];
    shadow_table.head[bucket] = index;
    shadow_table.entry[index].spec_id = spec_id;
    shadow_table.entry[index].dev = dev;
    shadow_table.entry[index].blockno = blockno;
    shadow_table.entry[index].state = SHADOW_ALLOCATED;
    shadow_table.entry[index].refs = 0;
    shadow_table.allocated++;
    if(shadow_table.allocated > shadow_table.peak)
      shadow_table.peak = shadow_table.allocated;
  }
  shadow_table.entry[index].refs++;
  memmove(shadow_table.entry[index].data, src, BSIZE);
  shadow_table.entry[index].refs--;
  release(&shadow_table.lock);
  return 0;
}

// The initial implementation has no version metadata to validate.  Returning
// zero means validation succeeded; publication will add conflict checks later.
int
shadow_validate(int spec_id)
{
  if(spec_id <= 0)
    return -1;
  return 0;
}

void
shadow_discard(int spec_id)
{
  int i, index, previous, next;

  if(spec_id <= 0)
    return;
  acquire(&shadow_table.lock);
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
  release(&shadow_table.lock);
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
