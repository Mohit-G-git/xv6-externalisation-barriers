#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "spinlock.h"
#include "proc.h"
#include "spec.h"

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
  uint generation;
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
     ctx->proc_refs != 0 || ctx->op_refs != 0)
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
  p->spec_id = id;
  release(&spectable.lock);
  return id;
}

int
spec_commit(struct proc *p)
{
  struct spec_ctx *ctx;
  int result = -1;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid){
    ctx->state = SPEC_COMMITTING;
    while(ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    // No speculative mutation is admitted until shadow storage and journal
    // publication are integrated. Thus this commits an empty context only.
    ctx->state = SPEC_COMMITTED;
    spec_drop_proc_locked(ctx, p);
    result = 0;
  }
  release(&spectable.lock);
  return result;
}

int
spec_abort(struct proc *p)
{
  struct spec_ctx *ctx;
  int result = -1;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid){
    ctx->state = SPEC_ABORTED;
    while(ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
    spec_drop_proc_locked(ctx, p);
    result = 0;
  }
  release(&spectable.lock);
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

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE && ctx->owner == p->pid){
    ctx->state = SPEC_ABORTED; // owner death aborts the shared context.
    while(ctx->op_refs != 0)
      sleep(ctx, &spectable.lock);
  }
  spec_drop_proc_locked(ctx, p);
  release(&spectable.lock);
}

int
spec_op_enter(struct proc *p, struct spec_ctx **out)
{
  struct spec_ctx *ctx;
  int result = -1;

  *out = 0;
  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && ctx->state == SPEC_ACTIVE){
    ctx->op_refs++;
    *out = ctx;
    result = 0;
  }
  release(&spectable.lock);
  return result;
}

void
spec_op_leave(struct spec_ctx *ctx)
{
  acquire(&spectable.lock);
  if(ctx->op_refs == 0){
    release(&spectable.lock);
    panic("spec_op_leave");
  }
  ctx->op_refs--;
  if(ctx->op_refs == 0)
    wakeup(ctx);
  spec_reclaim_locked(ctx);
  release(&spectable.lock);
}

// Until shadow writes and the externalization layer are integrated, reject
// mutations made by active speculative processes. This fail-closed guard is
// replaced by routing to private versions, not removed, during integration.
int
spec_mutation_allowed(struct proc *p)
{
  struct spec_ctx *ctx;
  int allowed = 1;

  acquire(&spectable.lock);
  ctx = spec_lookup_locked(p->spec_id);
  if(ctx != 0 && (ctx->state == SPEC_ACTIVE ||
                  ctx->state == SPEC_COMMITTING ||
                  ctx->state == SPEC_ABORTED))
    allowed = 0;
  else if(p->spec_id != 0)
    spec_drop_proc_locked(ctx, p);
  release(&spectable.lock);
  return allowed;
}
