#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "spec.h"
#include "sleeplock.h"
#include "fs.h"
#include "shadow.h"
#include "buf.h"

#define NSPEC 16
#define SPEC_SLOT_BITS 5
#define SPEC_SLOT_MASK ((1 << SPEC_SLOT_BITS) - 1)
#define SPEC_MAX_GENERATION 0x03ffffff

struct spec_ctx {
  int id;
  int owner;
  enum spec_state state;
  uint proc_refs;
  uint op_refs;
  uint file_refs;
  uint generation;
  int abort_only;
  int cleanup_busy;
};

static struct {
  struct spinlock lock;
  struct spec_ctx ctx[NSPEC];
} spectable;

static int
spec_slot(int id)
{
  int slot;

  slot = (id & SPEC_SLOT_MASK) - 1;
  if(id <= 0 || slot < 0 || slot >= NSPEC)
    return -1;
  return slot;
}

// Caller holds spectable.lock. Handles encode a generation and a slot so a
// process with a stale integer cannot accidentally refer to a reused entry.
static struct spec_ctx*
spec_lookup_locked(int id)
{
  int slot;
  struct spec_ctx *ctx;

  if((slot = spec_slot(id)) < 0)
    return 0;
  ctx = &spectable.ctx[slot];
  if(ctx->state == SPEC_FREE || ctx->id != id)
    return 0;
  return ctx;
}

static void
spec_reclaim_locked(struct spec_ctx *ctx)
{
  uint generation;

  if(ctx->state == SPEC_ACTIVE || ctx->state == SPEC_COMMITTING ||
     ctx->proc_refs != 0 || ctx->op_refs != 0 || ctx->file_refs != 0)
    return;
  generation = ctx->generation + 1;
  if(generation == 0 || generation > SPEC_MAX_GENERATION)
    generation = 1;
  memset(ctx, 0, sizeof(*ctx));
  ctx->generation = generation;
}

static void
spec_drop_proc_locked(struct spec_ctx *ctx, struct proc *p)
{
  if(p->spec_id == 0)
    return;
  if(ctx != 0 && ctx->proc_refs > 0){
    ctx->proc_refs--;
    spec_reclaim_locked(ctx);
  }
  p->spec_id = 0;
}

void
specinit(void)
{
  int i;

  initlock(&spectable.lock, "spectable");
  for(i = 0; i < NSPEC; i++){
    memset(&spectable.ctx[i], 0, sizeof(spectable.ctx[i]));
    spectable.ctx[i].generation = 1;
  }
}

int
spec_begin(struct proc *p)
{
  int i, id;
  struct spec_ctx *ctx;

  acquire(&spectable.lock);
  if(p->spec_id != 0){
    ctx = spec_lookup_locked(p->spec_id);
    if(ctx != 0 && (ctx->state == SPEC_ACTIVE ||
                    ctx->state == SPEC_COMMITTING)){
      release(&spectable.lock);
      return -1; // nested begin is not supported by this lifecycle layer.
    }
    spec_drop_proc_locked(ctx, p); // detach a stale terminal association.
  }

  for(i = 0; i < NSPEC; i++)
    if(spectable.ctx[i].state == SPEC_FREE)
      break;
  if(i == NSPEC){
    release(&spectable.lock);
    return -1;
  }

  ctx = &spectable.ctx[i];
  id = (int)((ctx->generation << SPEC_SLOT_BITS) | (i + 1));
  if(id <= 0){
    // Keep handles positive for the user ABI; restart the generation range.
    ctx->generation = 1;
    id = (int)((ctx->generation << SPEC_SLOT_BITS) | (i + 1));
  }
  ctx->id = id;
  ctx->owner = p->pid;
  ctx->state = SPEC_ACTIVE;
  ctx->proc_refs = 1;
  ctx->op_refs = 0;
  ctx->file_refs = 0;
  p->spec_id = id;
  release(&spectable.lock);
  return id;
}

int
spec_commit(struct proc *p)
{
  struct spec_ctx *ctx;
  int id = 0;
  int result = -1;
  int nblocks;
  int next, seen;
  uint epoch;
  struct shadow_snapshot snapshot;
  struct shadow_block block;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid &&
     !ctx->abort_only){
    ctx->state = SPEC_COMMITTING;
    id = ctx->id;
    result = 1;
  }
  release(&spectable.lock);

  if(result == 1){
    // A speculative pipe reader can otherwise sleep indefinitely while its
    // operation reference prevents this commit from draining.
    pipe_context_wakeup(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    while(ctx != 0 && ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    if(ctx == 0 || ctx->state != SPEC_COMMITTING || ctx->abort_only){
      if(ctx != 0 && ctx->state == SPEC_COMMITTING){
        ctx->state = SPEC_ACTIVE;
        wakeup(ctx);
      }
      release(&spectable.lock);
      return -1;
    }
    release(&spectable.lock);

    // Member 4's atomic publisher is not present. Freeze and preserve a
    // nonempty private set; never discard it or report a partial commit.
    if(shadow_freeze(id) < 0){
      acquire(&spectable.lock);
      ctx = spec_lookup_locked(id);
      if(ctx != 0 && ctx->state == SPEC_COMMITTING)
        ctx->state = SPEC_ACTIVE;
      if(ctx != 0)
        wakeup(ctx);
      release(&spectable.lock);
      return -1;
    }
    nblocks = shadow_count(id);
    if(nblocks < 0 || shadow_validate(id) < 0 ||
       shadow_snapshot_begin(id, &snapshot) < 0){
      if(shadow_thaw(id) < 0)
        panic("spec_commit: validation thaw");
      acquire(&spectable.lock);
      ctx = spec_lookup_locked(id);
      if(ctx != 0 && ctx->state == SPEC_COMMITTING)
        ctx->state = SPEC_ACTIVE;
      if(ctx != 0)
        wakeup(ctx);
      release(&spectable.lock);
      return -1;
    }
    seen = 0;
    while((next = shadow_snapshot_next(&snapshot, &block)) > 0){
      if(block.dev == 0 || block.blockno >= FSSIZE){
        seen = -1;
        break;
      }
      seen++;
    }
    if(next < 0 || seen < 0 || seen != snapshot.count){
      shadow_snapshot_end(&snapshot);
      if(shadow_thaw(id) < 0)
        panic("spec_commit: snapshot thaw");
      acquire(&spectable.lock);
      ctx = spec_lookup_locked(id);
      if(ctx != 0 && ctx->state == SPEC_COMMITTING)
        ctx->state = SPEC_ACTIVE;
      if(ctx != 0)
        wakeup(ctx);
      release(&spectable.lock);
      return -1;
    }
    shadow_snapshot_end(&snapshot);

    // xv6's begin_op() reserves MAXOPBLOCKS for one operation. Keep the
    // outer speculative commit atomic by rejecting a larger final set before
    // touching any real buffer.
    if(nblocks > MAXOPBLOCKS){
      if(shadow_thaw(id) < 0)
        panic("spec_commit: shadow thaw");
      acquire(&spectable.lock);
      ctx = spec_lookup_locked(id);
      if(ctx != 0 && ctx->state == SPEC_COMMITTING)
        ctx->state = SPEC_ACTIVE;
      if(ctx != 0)
        wakeup(ctx);
      release(&spectable.lock);
      return -1;
    }

    // Re-snapshot the frozen set and publish through the ordinary buffer
    // cache and log. The private image is never installed with bwrite().
    if(shadow_snapshot_begin(id, &snapshot) < 0)
      panic("spec_commit: publish snapshot");
    epoch = log_commit_epoch();
    if(nblocks > 0)
      begin_op();
    while((next = shadow_snapshot_next(&snapshot, &block)) > 0){
      struct buf *bp;

      bp = bread(block.dev, block.blockno);
      memmove(bp->data, block.data, BSIZE);
      log_write(bp);
      brelse(bp);
    }
    if(next < 0 || seen != nblocks)
      panic("spec_commit: publish snapshot changed");
    shadow_snapshot_end(&snapshot);
    if(nblocks > 0){
      end_op();
      log_wait_commit(epoch);
    }

    fs_spec_publish(id);
    shadow_discard(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    if(ctx == 0 || ctx->state != SPEC_COMMITTING){
      release(&spectable.lock);
      panic("spec_commit: state changed");
    }
    ctx->state = SPEC_COMMITTED;
    wakeup(ctx);
    release(&spectable.lock);
    spec_output_publish(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    spec_drop_proc_locked(ctx, p);
    release(&spectable.lock);
    result = 0;
  }
  return result;
}

int
spec_abort(struct proc *p)
{
  struct spec_ctx *ctx;
  int id = 0;
  int result = -1;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid){
    ctx->state = SPEC_ABORTED;
    ctx->cleanup_busy = 1;
    id = ctx->id;
    result = 0;
  }
  release(&spectable.lock);
  if(result == 0){
    pipe_context_wakeup(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    while(ctx != 0 && ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    release(&spectable.lock);
    fs_spec_discard(id);
    shadow_discard(id);
    spec_output_discard(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    if(ctx != 0){
      ctx->cleanup_busy = 0;
      wakeup(ctx);
    }
    spec_drop_proc_locked(ctx, p);
    release(&spectable.lock);
  }
  return result;
}

void
spec_fork_attach(struct proc *parent, struct proc *child)
{
  struct spec_ctx *ctx;

  child->spec_id = 0;
  if(parent->spec_id == 0)
    return;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(parent->spec_id);
  if(ctx != 0 && (ctx->state == SPEC_ACTIVE ||
                  ctx->state == SPEC_COMMITTING ||
                  ctx->state == SPEC_ABORTED)){
    ctx->proc_refs++;
    child->spec_id = parent->spec_id;
  } else {
    spec_drop_proc_locked(ctx, parent);
  }
  release(&spectable.lock);
}

void
spec_proc_exit(struct proc *p)
{
  struct spec_ctx *ctx;
  int id = 0;
  int aborted = 0;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid){
    ctx->state = SPEC_ABORTED; // owner death aborts the shared context.
    ctx->cleanup_busy = 1;
    id = ctx->id;
    aborted = 1;
  }
  release(&spectable.lock);
  if(aborted){
    pipe_context_wakeup(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    while(ctx != 0 && ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    release(&spectable.lock);
    fs_spec_discard(id);
    shadow_discard(id);
    spec_output_discard(id);
  }
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  while(!aborted && ctx != 0 && ctx->state == SPEC_ABORTED &&
        ctx->cleanup_busy)
    sleep(ctx, &spectable.lock);
  if(aborted && ctx != 0){
    ctx->cleanup_busy = 0;
    wakeup(ctx);
  }
  spec_drop_proc_locked(ctx, p);
  release(&spectable.lock);
}

int
spec_op_enter(struct proc *p, struct spec_op *op)
{
  struct spec_ctx *ctx;
  int result = -1;

  if(op == 0)
    return -1;
  memset(op, 0, sizeof(*op));
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_COMMITTED){
    spec_drop_proc_locked(ctx, p);
    ctx = 0;
  }
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && !ctx->abort_only){
    ctx->op_refs++;
    op->ctx = ctx;
    op->spec_id = ctx->id;
    op->active = 1;
    result = 0;
  }
  release(&spectable.lock);
  return result;
}

// Exit-time descriptor/cwd cleanup may release private inode references even
// for an abort-only context. It may not enter during publication or abort
// cleanup; wait for that terminal window to resolve first.
int
spec_op_enter_exit(struct proc *p, struct spec_op *op)
{
  struct spec_ctx *ctx;

  if(op == 0)
    return -1;
  memset(op, 0, sizeof(*op));
  acquire(&spectable.lock);
  for(;;){
    ctx = spec_lookup_locked(p->spec_id);
    if(ctx == 0)
      break;
    if(ctx->state == SPEC_COMMITTING ||
       (ctx->state == SPEC_ABORTED && ctx->cleanup_busy)){
      sleep(ctx, &spectable.lock);
      continue;
    }
    if(ctx->state == SPEC_ACTIVE){
      ctx->op_refs++;
      op->ctx = ctx;
      op->spec_id = ctx->id;
      op->active = 1;
      release(&spectable.lock);
      return 0;
    }
    if(ctx->state == SPEC_COMMITTED)
      spec_drop_proc_locked(ctx, p);
    break;
  }
  release(&spectable.lock);
  return -1;
}

void
spec_op_abort_only(struct spec_op *op)
{
  struct spec_ctx *ctx;

  if(op == 0 || !op->active || op->ctx == 0)
    panic("spec_op_abort_only");
  acquire(&spectable.lock);
  ctx = op->ctx;
  if(ctx->id != op->spec_id){
    release(&spectable.lock);
    panic("spec_op_abort_only state");
  }
  if(ctx->state == SPEC_ACTIVE || ctx->state == SPEC_COMMITTING)
    ctx->abort_only = 1;
  else if(ctx->state != SPEC_ABORTED){
    release(&spectable.lock);
    panic("spec_op_abort_only state");
  }
  release(&spectable.lock);
}

void
spec_op_leave(struct spec_op *op)
{
  struct spec_ctx *ctx;

  if(op == 0 || !op->active || op->ctx == 0)
    panic("spec_op_leave");
  acquire(&spectable.lock);
  ctx = op->ctx;
  if(ctx->id != op->spec_id){
    release(&spectable.lock);
    panic("spec_op_leave id");
  }
  if(ctx->op_refs == 0){
    release(&spectable.lock);
    panic("spec_op_leave");
  }
  ctx->op_refs--;
  if(ctx->op_refs == 0)
    wakeup(ctx);
  spec_reclaim_locked(ctx);
  op->ctx = 0;
  op->spec_id = 0;
  op->active = 0;
  release(&spectable.lock);
}

int
spec_file_attach(int spec_id)
{
  struct spec_ctx *ctx;
  int result = -1;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(spec_id);
  if(ctx != 0 && (ctx->state == SPEC_ACTIVE ||
                  ctx->state == SPEC_COMMITTING)){
    ctx->file_refs++;
    result = 0;
  }
  release(&spectable.lock);
  return result;
}

void
spec_file_detach(int spec_id)
{
  struct spec_ctx *ctx;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(spec_id);
  if(ctx != 0){
    if(ctx->file_refs == 0){
      release(&spectable.lock);
      panic("spec_file_detach");
    }
    ctx->file_refs--;
    spec_reclaim_locked(ctx);
  }
  release(&spectable.lock);
}

enum spec_state
spec_context_state(int spec_id)
{
  struct spec_ctx *ctx;
  enum spec_state state;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(spec_id);
  state = ctx == 0 ? SPEC_FREE : ctx->state;
  release(&spectable.lock);
  return state;
}
