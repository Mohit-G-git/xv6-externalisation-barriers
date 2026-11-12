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
  int parent_id;
  int child_id;
  uint depth;
};

static struct {
  struct spinlock lock;
  struct spec_ctx ctx[NSPEC];
} spectable;

#ifdef SPEC_DEBUG
static struct spinlock spec_trace_lock;
static uint spec_trace_sequence;
#endif

void
spec_trace(const char *event, int spec_id, int pid, uint dev, uint blockno,
           int expected, int actual)
{
#ifdef SPEC_DEBUG
  uint sequence;
  struct proc *p;

  acquire(&spec_trace_lock);
  sequence = ++spec_trace_sequence;
  release(&spec_trace_lock);
  if(pid < 0){
    p = myproc();
    pid = p == 0 ? 0 : p->pid;
  }
  cprintf("SPEC_EVENT seq=%d event=%s spec=%d pid=%d dev=%d block=%d "
          "expected=%d actual=%d\n",
          (int)sequence, event, spec_id, pid, (int)dev, (int)blockno,
          expected, actual);
#else
  (void)event;
  (void)spec_id;
  (void)pid;
  (void)dev;
  (void)blockno;
  (void)expected;
  (void)actual;
#endif
}

#ifdef SPEC_DEBUG
#define SPEC_TRACE(event, id, pid, dev, block, expected, actual) \
  spec_trace(event, id, pid, dev, block, expected, actual)
#else
#define SPEC_TRACE(event, id, pid, dev, block, expected, actual) \
  ((void)0)
#endif

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
     ctx->proc_refs != 0 || ctx->op_refs != 0 || ctx->file_refs != 0 ||
     ctx->child_id != 0)
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

static void
spec_release_inode(struct inode *ip)
{
  if(ip == 0)
    return;
  begin_op();
  iput(ip);
  end_op();
}

void
specinit(void)
{
  int i;

  initlock(&spectable.lock, "spectable");
#ifdef SPEC_DEBUG
  initlock(&spec_trace_lock, "spectrace");
  spec_trace_sequence = 0;
#endif
  for(i = 0; i < NSPEC; i++){
    memset(&spectable.ctx[i], 0, sizeof(spectable.ctx[i]));
    spectable.ctx[i].generation = 1;
  }
}

int
spec_begin(struct proc *p)
{
  int i, id, parent_id, depth;
  struct spec_ctx *ctx, *parent;
  struct inode *savedcwd;

  acquire(&spectable.lock);
  parent_id = p->spec_id;
  depth = p->spec_depth;
  parent = parent_id == 0 ? 0 : spec_lookup_locked(parent_id);
  if(parent_id != 0 &&
     (parent == 0 || (parent->state != SPEC_ACTIVE &&
                      parent->state != SPEC_COMMITTING))){
    if(parent != 0 && parent->state == SPEC_ABORTED && parent->cleanup_busy){
      release(&spectable.lock);
      return -1;
    }
    spec_drop_proc_locked(parent, p);
    p->spec_depth = 0;
    parent_id = 0;
    depth = 0;
    parent = 0;
  }
  if((parent_id == 0 && depth != 0) ||
     (parent_id != 0 && (parent == 0 || parent->state != SPEC_ACTIVE ||
                         parent->child_id != 0)) || depth >= NSPEC){
    release(&spectable.lock);
    return -1;
  }
  release(&spectable.lock);

  if(p->cwd == 0 || p->spec_cwd_stack[depth] != 0)
    return -1;
  savedcwd = idup(p->cwd);
  if(savedcwd == 0)
    return -1;

  acquire(&spectable.lock);
  parent = parent_id == 0 ? 0 : spec_lookup_locked(parent_id);
  if((parent_id != 0 && (parent == 0 || parent->state != SPEC_ACTIVE ||
                         parent->child_id != 0)) || p->spec_id != parent_id ||
     p->spec_depth != depth){
    release(&spectable.lock);
    spec_release_inode(savedcwd);
    return -1;
  }
  for(i = 0; i < NSPEC; i++)
    if(spectable.ctx[i].state == SPEC_FREE)
      break;
  if(i == NSPEC){
    release(&spectable.lock);
    spec_release_inode(savedcwd);
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
  ctx->parent_id = parent_id;
  ctx->child_id = 0;
  ctx->depth = depth;
  ctx->abort_only = 0;
  ctx->cleanup_busy = 0;
  if(parent != 0){
    parent->child_id = id;
    if(parent->proc_refs == 0)
      panic("spec_begin parent refs");
    parent->proc_refs--;
  }
  p->spec_id_stack[depth] = parent_id;
  p->spec_cwd_stack[depth] = savedcwd;
  p->spec_depth = depth + 1;
  p->spec_id = id;
  release(&spectable.lock);
  shadow_set_parent(id, parent_id);
  SPEC_TRACE("begin", id, p->pid, 0, 0, 0, 0);
  return id;
}

static int
spec_commit_nested(struct proc *p, int id, int parent_id)
{
  struct spec_ctx *ctx, *parent;
  int moved_procs, moved_files;

  SPEC_TRACE("nested_commit_begin", id, p->pid, 0, 0, 0, 0);
  pipe_context_wakeup(id);
  pipe_context_wakeup(parent_id);
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  parent = spec_lookup_locked(parent_id);
  while(ctx != 0 && parent != 0 &&
        (ctx->op_refs != 0 || parent->op_refs != 0)){
    if(ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    else
      sleep(parent, &spectable.lock);
    ctx = spec_lookup_locked(id);
    parent = spec_lookup_locked(parent_id);
  }
  if(ctx == 0 || parent == 0 || ctx->state != SPEC_COMMITTING ||
     parent->state != SPEC_COMMITTING || ctx->abort_only ||
     parent->child_id != id){
    if(ctx != 0 && ctx->state == SPEC_COMMITTING)
      ctx->state = SPEC_ACTIVE;
    if(parent != 0 && parent->state == SPEC_COMMITTING)
      parent->state = SPEC_ACTIVE;
    if(ctx != 0)
      wakeup(ctx);
    if(parent != 0)
      wakeup(parent);
    release(&spectable.lock);
    return -1;
  }
  release(&spectable.lock);

  if(shadow_freeze(parent_id) < 0)
    goto retry;
  if(shadow_freeze(id) < 0){
    if(shadow_thaw(parent_id) < 0)
      panic("nested commit parent thaw");
    goto retry;
  }
  if(shadow_validate(parent_id) < 0 || shadow_validate(id) < 0 ||
     spec_output_merge_check(id, parent_id) < 0 ||
     shadow_merge(id, parent_id) < 0){
    if(shadow_thaw(id) < 0 || shadow_thaw(parent_id) < 0)
      panic("nested commit validation thaw");
    goto retry;
  }
  if(spec_output_merge(id, parent_id) < 0)
    panic("nested commit output merge");
  fs_spec_merge(id, parent_id);
  if(proc_spec_scope_prepare(id, parent_id, 1) < 0)
    panic("nested commit process prepare");
  moved_files = file_context_merge(id, parent_id);
  if(shadow_thaw(parent_id) < 0)
    panic("nested commit parent thaw");
  proc_spec_scope_cwd_finish(id);

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  parent = spec_lookup_locked(parent_id);
  if(ctx == 0 || parent == 0 || ctx->state != SPEC_COMMITTING ||
     parent->state != SPEC_COMMITTING || parent->child_id != id){
    release(&spectable.lock);
    panic("nested commit state changed");
  }
  moved_procs = proc_spec_scope_reparent(id, parent_id);
  if(moved_procs != (int)ctx->proc_refs || moved_files != (int)ctx->file_refs)
    panic("nested commit references");
  parent->proc_refs += moved_procs;
  parent->file_refs += moved_files;
  ctx->proc_refs = 0;
  ctx->file_refs = 0;
  parent->child_id = 0;
  ctx->parent_id = 0;
  ctx->state = SPEC_COMMITTED;
  release(&spectable.lock);

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  parent = spec_lookup_locked(parent_id);
  if(parent == 0 || parent->state != SPEC_COMMITTING)
    panic("nested commit parent state");
  parent->state = SPEC_ACTIVE;
  wakeup(parent);
  if(ctx != 0){
    wakeup(ctx);
    spec_reclaim_locked(ctx);
  }
  release(&spectable.lock);
  SPEC_TRACE("nested_commit_done", id, p->pid, 0, 0, 0, 0);
  return 0;

retry:
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  parent = spec_lookup_locked(parent_id);
  if(ctx != 0 && ctx->state == SPEC_COMMITTING)
    ctx->state = SPEC_ACTIVE;
  if(parent != 0 && parent->state == SPEC_COMMITTING)
    parent->state = SPEC_ACTIVE;
  if(ctx != 0)
    wakeup(ctx);
  if(parent != 0)
    wakeup(parent);
  release(&spectable.lock);
  return -1;
}

int
spec_commit(struct proc *p)
{
  struct spec_ctx *ctx;
  int id = 0;
  int result = -1;
  int nblocks;
  int next, seen, moved;
  uint epoch;
  struct shadow_snapshot snapshot;
  struct shadow_block block;
  int parent_id = 0;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid &&
     !ctx->abort_only && ctx->child_id == 0){
    ctx->state = SPEC_COMMITTING;
    id = ctx->id;
    parent_id = ctx->parent_id;
    if(parent_id != 0){
      struct spec_ctx *parent = spec_lookup_locked(parent_id);
      if(parent == 0 || parent->state != SPEC_ACTIVE ||
         parent->child_id != id){
        ctx->state = SPEC_ACTIVE;
        release(&spectable.lock);
        return -1;
      }
      parent->state = SPEC_COMMITTING;
    }
    result = 1;
  }
  release(&spectable.lock);

  if(result == 1){
    if(parent_id != 0)
      return spec_commit_nested(p, id, parent_id);
    SPEC_TRACE("commit_begin", id, p->pid, 0, 0, 0, 0);
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

    // Freeze the private set before validating or publishing it.
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

    // Block new filesystem admissions before conflict validation and keep
    // exclusive ownership through journal completion and cache refresh.
    fs_gate_publish_begin();
    if(shadow_validate_versions(id) < 0){
      fs_gate_publish_end();
      if(shadow_thaw(id) < 0)
        panic("spec_commit: conflict thaw");
      acquire(&spectable.lock);
      ctx = spec_lookup_locked(id);
      if(ctx != 0 && ctx->state == SPEC_COMMITTING)
        ctx->state = SPEC_ACTIVE;
      if(ctx != 0)
        wakeup(ctx);
      release(&spectable.lock);
      SPEC_TRACE("commit_conflict", id, p->pid, 0, 0, 0, 0);
      return -1;
    }

    // Reserve the entire frozen set against the actual journal capacity.
    // Failure is reported before any base buffer can be modified.
    if(log_begin_op_commit(&epoch, nblocks) < 0){
      fs_gate_publish_end();
      if(shadow_thaw(id) < 0)
        panic("spec_commit: capacity thaw");
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
    seen = 0;
    while((next = shadow_snapshot_next(&snapshot, &block)) > 0){
      struct buf *bp;

      bp = bread(block.dev, block.blockno);
      memmove(bp->data, block.data, BSIZE);
      log_write(bp);
      brelse(bp);
      seen++;
    }
    if(next < 0 || seen != nblocks)
      panic("spec_commit: publish snapshot changed");
    shadow_snapshot_end(&snapshot);
    log_end_op_commit();
    log_wait_commit(epoch);

    // Publish a new base generation for every committed block. Without this,
    // a later transaction could validate against a version from before this
    // commit and overwrite the newer contents.
    if(shadow_snapshot_begin(id, &snapshot) < 0)
      panic("spec_commit: generation snapshot");
    seen = 0;
    while((next = shadow_snapshot_next(&snapshot, &block)) > 0){
      shadow_note_base_write(block.dev, block.blockno);
      seen++;
    }
    shadow_snapshot_end(&snapshot);
    if(next < 0 || seen != nblocks)
      panic("spec_commit: generation snapshot changed");

    fs_spec_publish(id);
    shadow_discard(id);
    spec_output_publish(id);
    fs_gate_publish_end();
    if(proc_spec_scope_prepare(id, 0, 0) <= 0)
      panic("spec_commit process prepare");
    proc_spec_scope_cwd_finish(id);
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(id);
    if(ctx == 0 || ctx->state != SPEC_COMMITTING){
      release(&spectable.lock);
      panic("spec_commit: state changed");
    }
    moved = proc_spec_scope_reparent(id, 0);
    if(moved != (int)ctx->proc_refs)
      panic("spec_commit process references");
    ctx->proc_refs = 0;
    ctx->state = SPEC_COMMITTED;
    wakeup(ctx);
    release(&spectable.lock);
    SPEC_TRACE("commit_done", id, p->pid, 0, 0, 0, 0);
    result = 0;
  }
  return result;
}

static void
spec_abort_context(int id, int parent_id)
{
  struct spec_ctx *ctx;
  struct spec_ctx *parent;
  int moved;

  SPEC_TRACE("abort", id, -1, 0, 0, 0, 0);
  pipe_context_wakeup(id);
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  while(ctx != 0 && ctx->op_refs != 0){
    sleep(ctx, &spectable.lock);
    ctx = spec_lookup_locked(id);
  }
  release(&spectable.lock);

  fs_spec_discard(id);
  shadow_discard(id);
  spec_output_discard(id);
  if(proc_spec_scope_prepare(id, parent_id, 0) <= 0)
    panic("spec_abort process prepare");
  proc_spec_scope_cwd_finish(id);

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(id);
  parent = parent_id == 0 ? 0 : spec_lookup_locked(parent_id);
  if(ctx == 0 || ctx->state != SPEC_ABORTED || !ctx->cleanup_busy ||
     (parent_id != 0 && (parent == 0 || parent->child_id != id)))
    panic("spec_abort state");
  if(parent_id == 0){
    moved = proc_spec_scope_abort_outer(id, myproc()->pid);
    if(moved != 1 || ctx->proc_refs < (uint)moved)
      panic("spec_abort owner references");
    ctx->proc_refs -= moved;
  } else {
    moved = proc_spec_scope_reparent(id, parent_id);
    if(moved != (int)ctx->proc_refs)
      panic("spec_abort process references");
    ctx->proc_refs = 0;
  }
  if(parent != 0){
    parent->proc_refs += moved;
    parent->child_id = 0;
  }
  ctx->parent_id = 0;
  ctx->cleanup_busy = 0;
  wakeup(ctx);
  if(parent != 0)
    wakeup(parent);
  spec_reclaim_locked(ctx);
  release(&spectable.lock);
  SPEC_TRACE("cleanup", id, -1, 0, 0, 0, 0);
}

int
spec_abort(struct proc *p)
{
  struct spec_ctx *ctx, *parent;
  int id, parent_id;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx == 0 || ctx->state != SPEC_ACTIVE || ctx->owner != p->pid ||
     ctx->child_id != 0){
    release(&spectable.lock);
    return -1;
  }
  parent_id = ctx->parent_id;
  parent = parent_id == 0 ? 0 : spec_lookup_locked(parent_id);
  if(parent_id != 0 && (parent == 0 || parent->state != SPEC_ACTIVE ||
                        parent->child_id != ctx->id)){
    release(&spectable.lock);
    return -1;
  }
  id = ctx->id;
  ctx->state = SPEC_ABORTED;
  ctx->cleanup_busy = 1;
  release(&spectable.lock);
  spec_abort_context(id, parent_id);
  return 0;
}

void
spec_fork_attach(struct proc *parent, struct proc *child)
{
  struct spec_ctx *ctx;
  int i;

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
  if(child->spec_id != 0){
    child->spec_depth = parent->spec_depth;
    for(i = 0; i < parent->spec_depth; i++){
      child->spec_id_stack[i] = parent->spec_id_stack[i];
      child->spec_cwd_stack[i] = idup(parent->spec_cwd_stack[i]);
      if(child->spec_cwd_stack[i] == 0)
        panic("spec_fork_attach cwd");
    }
    SPEC_TRACE("fork_attach", child->spec_id, child->pid, 0, 0, 0, 0);
  }
}

void
spec_proc_exit(struct proc *p)
{
  struct spec_ctx *ctx;
  int id, parent_id, child_id, owner;

  while(p->spec_id != 0){
    acquire(&spectable.lock);
    ctx = spec_lookup_locked(p->spec_id);
    if(ctx == 0){
      spec_drop_proc_locked(0, p);
      release(&spectable.lock);
      break;
    }
    if(ctx->state == SPEC_COMMITTING ||
       (ctx->state == SPEC_ABORTED && ctx->cleanup_busy)){
      sleep(ctx, &spectable.lock);
      release(&spectable.lock);
      continue;
    }
    child_id = ctx->child_id;
    owner = ctx->owner == p->pid;
    if(ctx->state == SPEC_ACTIVE && !owner){
      spec_drop_proc_locked(ctx, p);
      release(&spectable.lock);
      break;
    }
    if(child_id != 0){
      struct spec_ctx *child = spec_lookup_locked(child_id);
      if(child != 0 && child->state == SPEC_ACTIVE){
        id = ctx->id;
        child->state = SPEC_ABORTED;
        child->cleanup_busy = 1;
        release(&spectable.lock);
        SPEC_TRACE("owner_exit_nested_abort", child_id, p->pid, 0, 0, 0, 0);
        spec_abort_context(child_id, id);
        continue;
      }
      sleep(ctx, &spectable.lock);
      release(&spectable.lock);
      continue;
    }
    id = ctx->id;
    parent_id = ctx->parent_id;
    if(ctx->state == SPEC_ACTIVE && owner){
      ctx->state = SPEC_ABORTED;
      ctx->cleanup_busy = 1;
      release(&spectable.lock);
      SPEC_TRACE("owner_exit_abort", id, p->pid, 0, 0, 0, 0);
      spec_abort_context(id, parent_id);
      continue;
    }
    spec_drop_proc_locked(ctx, p);
    release(&spectable.lock);
    break;
  }
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

int
spec_context_visible(int owner_id, int viewer_id)
{
  struct spec_ctx *ctx;
  int current, depth, visible;

  if(owner_id <= 0 || viewer_id <= 0)
    return 0;
  acquire(&spectable.lock);
  current = viewer_id;
  visible = 0;
  for(depth = 0; depth < NSPEC && current > 0; depth++){
    if(current == owner_id){
      visible = 1;
      break;
    }
    ctx = spec_lookup_locked(current);
    if(ctx == 0)
      break;
    current = ctx->parent_id;
  }
  release(&spectable.lock);
  return visible;
}
