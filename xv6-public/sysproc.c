#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spec.h"
#include "shadow.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_xbegin(void)
{
  return spec_begin(myproc());
}

int
sys_xcommit(void)
{
  return spec_commit(myproc());
}

int
sys_xabort(void)
{
  return spec_abort(myproc());
}

int
sys_xstats(void)
{
  struct shadow_metrics stats;

  shadow_metrics_get(&stats);
  cprintf("SHADOW_METRICS,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
          (int)stats.allocated, (int)stats.peak, (int)stats.capacity,
          (int)stats.allocation_failures, (int)stats.lookups,
          (int)stats.lookup_hits, (int)stats.lookup_misses,
          (int)stats.bucket_acquires, (int)stats.bucket_contention,
          (int)stats.data_acquires);
  return 0;
}
