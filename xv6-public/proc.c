#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "spec.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->spec_id = 0;
  p->spec_depth = 0;
  memset(p->spec_id_stack, 0, sizeof(p->spec_id_stack));
  memset(p->spec_cwd_stack, 0, sizeof(p->spec_cwd_stack));
  p->spec_transition_id = 0;
  p->spec_transition_cwd = 0;
  p->spec_transition_oldcwd = 0;
  p->spec_transition_savedcwd = 0;
  p->spec_transition_drop_saved = 0;
  p->spec_fault = 0;
  p->fs_error = 0;
  p->fs_gate_depth = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));
  spec_fork_attach(curproc, np);

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Prepare process-local cwd changes while the speculation contexts are
// frozen. The lifecycle layer keeps the context state non-active until it has
// atomically reparented process membership and released the old references.
int
proc_spec_scope_prepare(int spec_id, int parent_id, int merge)
{
  struct proc *members[NPROC];
  struct proc *p;
  struct inode *oldcwd, *savedcwd, *newcwd;
  int i, n;

  n = 0;
  acquire(&ptable.lock);
  for(i = 0; i < NPROC; i++){
    p = &ptable.proc[i];
    if(p->state == UNUSED || p->spec_id != spec_id)
      continue;
    if(p->spec_depth <= 0 || p->spec_transition_id != 0)
      panic("spec_scope_prepare state");
    p->spec_transition_id = spec_id;
    p->spec_transition_oldcwd = p->cwd;
    p->spec_transition_savedcwd = p->spec_cwd_stack[p->spec_depth - 1];
    p->spec_transition_drop_saved = parent_id > 0 && merge;
    p->spec_transition_cwd = 0;
    members[n++] = p;
  }
  release(&ptable.lock);

  for(i = 0; i < n; i++){
    p = members[i];
    oldcwd = p->spec_transition_oldcwd;
    savedcwd = p->spec_transition_savedcwd;
    if(oldcwd == 0 || savedcwd == 0)
      panic("spec_scope_prepare cwd");
    if(parent_id > 0 && merge){
      newcwd = iget_context(oldcwd->dev, oldcwd->inum, parent_id);
      if(newcwd == 0)
        panic("spec_scope_prepare rebase");
    } else
      newcwd = savedcwd;
    acquire(&ptable.lock);
    if(p->spec_transition_id != spec_id){
      release(&ptable.lock);
      panic("spec_scope_prepare raced");
    }
    p->spec_transition_cwd = newcwd;
    release(&ptable.lock);
  }
  return n;
}

// Caller holds the speculation registry lock, preventing exit/fork reference
// changes while process IDs and their context reference counts are updated.
int
proc_spec_scope_reparent(int spec_id, int parent_id)
{
  struct proc *p;
  int i, n;

  n = 0;
  acquire(&ptable.lock);
  for(i = 0; i < NPROC; i++){
    p = &ptable.proc[i];
    if(p->state == UNUSED || p->spec_id != spec_id)
      continue;
    if(p->spec_depth <= 0 ||
       p->spec_id_stack[p->spec_depth - 1] != parent_id)
      panic("spec_scope_reparent state");
    p->spec_id = parent_id;
    p->spec_id_stack[p->spec_depth - 1] = 0;
    p->spec_cwd_stack[p->spec_depth - 1] = 0;
    p->spec_depth--;
    n++;
  }
  release(&ptable.lock);
  return n;
}

// On outer abort, detach the owner that requested the abort but keep forked
// participants associated with the aborted context. Their syscalls remain
// blocked until they explicitly start a new speculation or exit.
int
proc_spec_scope_abort_outer(int spec_id, int owner_pid)
{
  struct proc *p;
  int i, detached;

  detached = 0;
  acquire(&ptable.lock);
  for(i = 0; i < NPROC; i++){
    p = &ptable.proc[i];
    if(p->state == UNUSED || p->spec_id != spec_id)
      continue;
    if(p->spec_depth != 1 || p->spec_id_stack[0] != 0 ||
       p->spec_cwd_stack[0] == 0)
      panic("spec_scope_abort_outer state");
    p->spec_id_stack[0] = 0;
    p->spec_cwd_stack[0] = 0;
    p->spec_depth = 0;
    if(p->pid == owner_pid){
      p->spec_id = 0;
      detached++;
    }
  }
  release(&ptable.lock);
  return detached;
}

void
proc_spec_scope_cwd_finish(int spec_id)
{
  struct proc *p;
  struct inode *oldcwd, *savedcwd;
  int i, drop_saved;

  for(i = 0; i < NPROC; i++){
    acquire(&ptable.lock);
    p = &ptable.proc[i];
    if(p->state == UNUSED || p->spec_transition_id != spec_id){
      release(&ptable.lock);
      continue;
    }
    oldcwd = p->spec_transition_oldcwd;
    savedcwd = p->spec_transition_savedcwd;
    drop_saved = p->spec_transition_drop_saved;
    if(p->spec_transition_cwd == 0)
      panic("spec_scope_cwd_finish state");
    p->cwd = p->spec_transition_cwd;
    p->spec_transition_id = 0;
    p->spec_transition_oldcwd = 0;
    p->spec_transition_savedcwd = 0;
    p->spec_transition_cwd = 0;
    p->spec_transition_drop_saved = 0;
    release(&ptable.lock);
    if(oldcwd != 0){
      begin_op();
      iput(oldcwd);
      end_op();
    }
    if(savedcwd != 0 && drop_saved){
      begin_op();
      iput(savedcwd);
      end_op();
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct spec_op exitop;
  int exitref;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  exitref = 0;
  if(curproc->spec_id != 0){
    if(spec_op_enter_exit(curproc, &exitop) == 0)
      exitref = 1;
    else
      spec_proc_exit(curproc);
  }

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  if(exitref)
    spec_op_leave(&exitop);
  spec_proc_exit(curproc);
  proc_spec_stack_cleanup(curproc);

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

void
proc_spec_stack_cleanup(struct proc *p)
{
  struct inode *saved[16];
  int i, depth;

  depth = p->spec_depth;
  if(depth < 0 || depth > 16)
    panic("spec stack depth");
  for(i = 0; i < depth; i++){
    saved[i] = p->spec_cwd_stack[i];
    p->spec_cwd_stack[i] = 0;
    p->spec_id_stack[i] = 0;
  }
  p->spec_depth = 0;
  for(i = 0; i < depth; i++)
    if(saved[i] != 0){
      begin_op();
      iput(saved[i]);
      end_op();
    }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->spec_id = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
